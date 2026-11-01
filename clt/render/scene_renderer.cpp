#include "scene_renderer.hpp"

#include "clt/network/connection.hpp"
#include "clt/render/upload.hpp"
#include "clt/view/camera.hpp"
#include "srv/shared/spatial/index.hpp"
#include "srv/shared/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace clt::scene_renderer
{
    namespace
    {
        using namespace vk::util::math;

        struct instance_data {
            mat4 model;
            mat4 normal;
        };

        struct prepared_instance {
            vec3 center;
            float radius = 0;
        };

        struct gpu_mesh {
            std::uint32_t id = 0;
            vk::buffer::resource vertices;
            vk::buffer::resource indices;
            vk::buffer::resource instance_buffer;
            vk::buffer::resource visible_buffer;
            vk::DescriptorSet instance_set{};
            std::vector<prepared_instance> instances;
            std::vector<instance_data> transforms;
            std::vector<std::uint32_t> visible;
            spatial::index visibility_index;
            vec3 center;
            float radius = 0;
            std::uint32_t index_count = 0;
        };

        static_assert(sizeof(instance_data) == 128);
        static_assert(sizeof(::scene::vertex) == 32);

        struct state {
            std::shared_ptr<const ::scene::state> snapshot;
            std::vector<gpu_mesh> meshes;
            vk::DescriptorSetLayout instance_layout{};
            vk::DescriptorPool instance_pool{};
            draw_stats timings{};

            vk::RenderPass render_pass{};
            vk::PipelineLayout layout{};
            vk::Pipeline pipeline{};

            vk::image::resource depth;
            vk::ImageView depth_view{};

            std::vector<vk::ImageView> views;
            std::vector<vk::Framebuffer> framebuffers;

            std::array<std::string, 2> shaders;
        };

        state& get()
        {
            static state data;
            return data;
        }

        void destroy_meshes(std::vector<gpu_mesh>& meshes)
        {
            for (auto& mesh : meshes)
            {
                vk::buffer::destroy(mesh.vertices);
                vk::buffer::destroy(mesh.indices);
                if (mesh.instance_buffer.handle)
                {
                    vk::buffer::destroy(mesh.instance_buffer);
                }
                if (mesh.visible_buffer.handle)
                {
                    vk::buffer::destroy(mesh.visible_buffer);
                }
            }

            meshes.clear();
        }

        // The checked-in SPIR-V can be older than the GLSL source even when
        // its file timestamp is newer (for example after git checkout).
        bool has_instance_inputs(const std::string& path)
        {
            if (!std::filesystem::exists(path))
            {
                return false;
            }

            const auto words = vk::shader::read(path);
            bool model = false;
            bool normal = false;

            for (std::size_t i = 5; i < words.size();)
            {
                const auto count = words[i] >> 16;
                const auto opcode = words[i] & 0xffffu;
                if (!count || count > words.size() - i)
                {
                    return false;
                }

                // SPIR-V OpDecorate with Location decoration (30).
                if (opcode == 71 && count >= 4 && words[i + 2] == 30)
                {
                    model |= words[i + 3] == 2;
                    normal |= words[i + 3] == 6;
                }
                i += count;
            }

            // The new shader reads matrices from a storage buffer; location
            // 2 is a single uint and location 6 must no longer be present.
            return model && !normal;
        }

        void update_shader(const std::string& name, bool instanced = false)
        {
            const std::filesystem::path binary(name);
            if (binary.extension() != ".spv")
            {
                return;
            }

            auto source = binary;
            source.replace_extension();
            const bool valid = std::filesystem::exists(binary) &&
                (!instanced || has_instance_inputs(name));

            if (!std::filesystem::exists(source))
            {
                if (!valid)
                {
                    throw std::runtime_error("Missing compatible shader: " + name);
                }
                return;
            }

            if (valid && std::filesystem::last_write_time(binary) >=
                         std::filesystem::last_write_time(source))
            {
                return;
            }

            // Configured shader paths are local application resources.
            if (name.find_first_of("\"'$;`\\") != std::string::npos)
            {
                throw std::runtime_error("Unsupported shader path characters");
            }

            const std::string command =
                "glslangValidator -V \"" + source.string() + "\" -o \"" + name + "\"";
            if (std::system(command.c_str()) != 0)
            {
                throw std::runtime_error("Shader compilation failed: " + source.string());
            }

            if (instanced && !has_instance_inputs(name))
            {
                throw std::runtime_error("Compiled shader lacks instance attributes: " + name);
            }
        }

        struct plane {
            vec3 normal;
            float offset;
            float magnitude;
        };

        std::array<plane, 6> frustum(const mat4& m)
        {
            const auto make = [&](int row, float sign)
            {
                plane p{
                    {m(3, 0) + sign * m(row, 0),
                     m(3, 1) + sign * m(row, 1),
                     m(3, 2) + sign * m(row, 2)},
                    m(3, 3) + sign * m(row, 3), 0
                };
                p.magnitude = length(p.normal);
                return p;
            };
            plane near{
                {m(2, 0), m(2, 1), m(2, 2)}, m(2, 3), 0
            };
            near.magnitude = length(near.normal);
            return {make(0, 1), make(0, -1), make(1, 1), make(1, -1),
                    near, make(2, -1)};
        }

        bool visible(const std::array<plane, 6>& planes, vec3 center, float radius)
        {
            for (const auto& p : planes)
            {
                if (dot(p.normal, center) + p.offset < -radius * p.magnitude)
                {
                    return false;
                }
            }
            return true;
        }

        void create_pipeline()
        {
            auto& data = get();

            update_shader(data.shaders[0], true);
            update_shader(data.shaders[1]);
            const auto vertex_code = vk::shader::read(data.shaders[0]);
            const auto fragment_code = vk::shader::read(data.shaders[1]);

            vk::ShaderModule vertex_shader{};
            vk::ShaderModule fragment_shader{};

            try
            {
                vertex_shader = vk::shader::create(vertex_code);
                fragment_shader = vk::shader::create(fragment_code);

                vk::PushConstantRange range{};
                range.stageFlags = vk::ShaderStageFlagBits::eVertex;
                range.offset = 0;
                range.size = sizeof(mat4);

                vk::PipelineLayoutCreateInfo layout_info{};
                layout_info.pushConstantRangeCount = 1;
                layout_info.pPushConstantRanges = &range;
                layout_info.setLayoutCount = 1;
                layout_info.pSetLayouts = &data.instance_layout;

                data.layout = vk::pipeline_layout::create(layout_info);

                std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};

                stages[0].stage = vk::ShaderStageFlagBits::eVertex;
                stages[0].module = vertex_shader;
                stages[0].pName = "main";

                stages[1].stage = vk::ShaderStageFlagBits::eFragment;
                stages[1].module = fragment_shader;
                stages[1].pName = "main";

                std::array<vk::VertexInputBindingDescription, 2> bindings{};
                bindings[0] = vk::VertexInputBindingDescription{0, sizeof(::scene::vertex), vk::VertexInputRate::eVertex};
                bindings[1] = vk::VertexInputBindingDescription{
                    1, sizeof(std::uint32_t), vk::VertexInputRate::eInstance
                };

                std::array<vk::VertexInputAttributeDescription, 3> attributes{};
                attributes[0] = vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32B32Sfloat,
                                 offsetof(::scene::vertex, position)};
                attributes[1] = vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32B32Sfloat,
                                 offsetof(::scene::vertex, normal)};
                attributes[2] = vk::VertexInputAttributeDescription{
                    2, 1, vk::Format::eR32Uint, 0
                };

                vk::PipelineVertexInputStateCreateInfo vertex_input{};
                vertex_input.vertexBindingDescriptionCount = bindings.size();
                vertex_input.pVertexBindingDescriptions = bindings.data();
                vertex_input.vertexAttributeDescriptionCount = attributes.size();
                vertex_input.pVertexAttributeDescriptions = attributes.data();

                vk::PipelineInputAssemblyStateCreateInfo assembly{};
                assembly.topology = vk::PrimitiveTopology::eTriangleList;

                vk::PipelineViewportStateCreateInfo viewport{};
                viewport.viewportCount = 1;
                viewport.scissorCount = 1;

                vk::PipelineRasterizationStateCreateInfo rasterization{};
                rasterization.polygonMode = vk::PolygonMode::eFill;
                rasterization.cullMode = vk::CullModeFlagBits::eNone;
                rasterization.frontFace = vk::FrontFace::eCounterClockwise;
                rasterization.lineWidth = 1.0f;

                vk::PipelineMultisampleStateCreateInfo multisample{};
                multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

                vk::PipelineDepthStencilStateCreateInfo depth{};
                depth.depthTestEnable = VK_TRUE;
                depth.depthWriteEnable = VK_TRUE;
                depth.depthCompareOp = vk::CompareOp::eLess;

                vk::PipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask =
                    vk::ColorComponentFlagBits::eR |
                    vk::ColorComponentFlagBits::eG |
                    vk::ColorComponentFlagBits::eB |
                    vk::ColorComponentFlagBits::eA;

                vk::PipelineColorBlendStateCreateInfo blending{};
                blending.attachmentCount = 1;
                blending.pAttachments = &blend;

                std::array<vk::DynamicState, 2> dynamic_states{
                    vk::DynamicState::eViewport,
                    vk::DynamicState::eScissor
                };

                vk::PipelineDynamicStateCreateInfo dynamic{};
                dynamic.dynamicStateCount = dynamic_states.size();
                dynamic.pDynamicStates = dynamic_states.data();

                vk::GraphicsPipelineCreateInfo info{};
                info.stageCount = stages.size();
                info.pStages = stages.data();
                info.pVertexInputState = &vertex_input;
                info.pInputAssemblyState = &assembly;
                info.pViewportState = &viewport;
                info.pRasterizationState = &rasterization;
                info.pMultisampleState = &multisample;
                info.pDepthStencilState = &depth;
                info.pColorBlendState = &blending;
                info.pDynamicState = &dynamic;
                info.layout = data.layout;
                info.renderPass = data.render_pass;
                info.subpass = 0;

                data.pipeline = vk::pipeline::graphics(info);
            }
            catch (...)
            {
                vk::shader::destroy(vertex_shader);
                vk::shader::destroy(fragment_shader);
                throw;
            }

            vk::shader::destroy(vertex_shader);
            vk::shader::destroy(fragment_shader);
        }

        void create_render_pass()
        {
            auto& data = get();

            std::array<vk::AttachmentDescription, 2> attachments{};

            attachments[0].format = vk::swapchain::get()->format;
            attachments[0].samples = vk::SampleCountFlagBits::e1;
            attachments[0].loadOp = vk::AttachmentLoadOp::eClear;
            attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
            attachments[0].initialLayout = vk::ImageLayout::eUndefined;
            attachments[0].finalLayout = vk::ImageLayout::ePresentSrcKHR;

            attachments[1].format = vk::Format::eD32Sfloat;
            attachments[1].samples = vk::SampleCountFlagBits::e1;
            attachments[1].loadOp = vk::AttachmentLoadOp::eClear;
            attachments[1].storeOp = vk::AttachmentStoreOp::eDontCare;
            attachments[1].initialLayout = vk::ImageLayout::eUndefined;
            attachments[1].finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

            vk::AttachmentReference color{};
            color.attachment = 0;
            color.layout = vk::ImageLayout::eColorAttachmentOptimal;

            vk::AttachmentReference depth{};
            depth.attachment = 1;
            depth.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

            vk::SubpassDescription subpass{};
            subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color;
            subpass.pDepthStencilAttachment = &depth;

            vk::SubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask =
                vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eEarlyFragmentTests;
            dependency.dstStageMask = dependency.srcStageMask;
            dependency.dstAccessMask =
                vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;

            vk::RenderPassCreateInfo info{};
            info.attachmentCount = attachments.size();
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;

            data.render_pass = vk::render_pass::create(info);
        }

        void create_depth()
        {
            auto& data = get();
            const auto extent = vk::swapchain::get()->extent;

            vk::ImageCreateInfo info{};
            info.imageType = vk::ImageType::e2D;
            info.format = vk::Format::eD32Sfloat;
            info.extent = vk::Extent3D{extent.width, extent.height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = vk::SampleCountFlagBits::e1;
            info.tiling = vk::ImageTiling::eOptimal;
            info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
            info.sharingMode = vk::SharingMode::eExclusive;
            info.initialLayout = vk::ImageLayout::eUndefined;

            data.depth = vk::image::create(
                info,
                vk::MemoryPropertyFlagBits::eDeviceLocal
            );

            vk::ImageViewCreateInfo view{};
            view.image = data.depth.handle;
            view.viewType = vk::ImageViewType::e2D;
            view.format = info.format;
            view.subresourceRange = {
                vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1
            };

            data.depth_view = vk::image_view::create(view);
        }
    } // namespace

    void init(const char* vertex_path, const char* fragment_path)
    {
        if (!vertex_path || !fragment_path || !*vertex_path || !*fragment_path)
        {
            throw std::invalid_argument("Scene shader paths must not be empty");
        }

        get().shaders = {vertex_path, fragment_path};

        vk::DescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = vk::DescriptorType::eStorageBuffer;
        binding.descriptorCount = 1;
        binding.stageFlags = vk::ShaderStageFlagBits::eVertex;

        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = 1;
        info.pBindings = &binding;
        get().instance_layout =
            vk::device::get()->handle.createDescriptorSetLayout(info);
    }

    draw_stats last_stats()
    {
        return get().timings;
    }

    void release_frame_resources()
    {
        auto& data = get();

        vk::pipeline::destroy(data.pipeline);
        data.pipeline = nullptr;

        vk::pipeline_layout::destroy(data.layout);
        data.layout = nullptr;

        for (auto framebuffer : data.framebuffers)
        {
            vk::framebuffer::destroy(framebuffer);
        }

        data.framebuffers.clear();

        for (auto view : data.views)
        {
            vk::image_view::destroy(view);
        }

        data.views.clear();

        vk::image_view::destroy(data.depth_view);
        data.depth_view = nullptr;

        if (data.depth.handle)
        {
            vk::image::destroy(data.depth);
        }

        vk::render_pass::destroy(data.render_pass);
        data.render_pass = nullptr;
    }

    void create_frame_resources()
    {
        auto& data = get();
        const auto extent = vk::swapchain::get()->extent;

        try
        {
            create_render_pass();
            create_depth();

            for (auto image : vk::swapchain::get()->images)
            {
                vk::ImageViewCreateInfo view{};
                view.image = image;
                view.viewType = vk::ImageViewType::e2D;
                view.format = vk::swapchain::get()->format;
                view.subresourceRange = {
                    vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
                };

                auto color_view = vk::image_view::create(view);
                data.views.push_back(color_view);

                std::array<vk::ImageView, 2> attachments{
                    color_view,
                    data.depth_view
                };

                vk::FramebufferCreateInfo framebuffer{};
                framebuffer.renderPass = data.render_pass;
                framebuffer.attachmentCount = attachments.size();
                framebuffer.pAttachments = attachments.data();
                framebuffer.width = extent.width;
                framebuffer.height = extent.height;
                framebuffer.layers = 1;

                data.framebuffers.push_back(
                    vk::framebuffer::create(framebuffer)
                );
            }

            create_pipeline();
        }
        catch (...)
        {
            release_frame_resources();
            throw;
        }
    }

    void append_instance(gpu_mesh& mesh, const ::scene::instance& instance)
    {
        const auto& pose = instance.pose;
        const vec3 center{
            mesh.center.x * pose.scale.x,
            mesh.center.y * pose.scale.y,
            mesh.center.z * pose.scale.z
        };
        const vec3 inverse{
            pose.scale.x ? 1.0f / pose.scale.x : 0.0f,
            pose.scale.y ? 1.0f / pose.scale.y : 0.0f,
            pose.scale.z ? 1.0f / pose.scale.z : 0.0f
        };
        const float max_scale = std::max({
            std::abs(pose.scale.x), std::abs(pose.scale.y), std::abs(pose.scale.z)
        });
        mesh.instances.push_back({
            pose.position + rotate(pose.rotation, center),
            mesh.radius * max_scale
        });
        mesh.transforms.push_back({
            trs(pose.position, pose.rotation, pose.scale),
            rotation(pose.rotation) * scale(inverse)
        });
    }

    void bind_instances(gpu_mesh& mesh)
    {
        if (!mesh.instance_set || !mesh.instance_buffer.handle)
        {
            return;
        }

        vk::DescriptorBufferInfo buffer{};
        buffer.buffer = mesh.instance_buffer.handle;
        buffer.offset = 0;
        buffer.range = VK_WHOLE_SIZE;

        vk::WriteDescriptorSet write{};
        write.dstSet = mesh.instance_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.pBufferInfo = &buffer;
        vk::device::get()->handle.updateDescriptorSets({write}, {});
    }

    void refresh_instances(gpu_mesh& mesh, std::size_t first = 0)
    {
        if (mesh.instances.empty())
        {
            return;
        }

        std::vector<spatial::bounds> boxes;
        boxes.reserve(mesh.instances.size() - first);
        for (std::size_t i = first; i < mesh.instances.size(); ++i)
        {
            const auto& instance = mesh.instances[i];
            const vec3 radius{instance.radius, instance.radius, instance.radius};
            boxes.push_back({
                instance.center - radius,
                instance.center + radius
            });
        }

        if (first)
        {
            mesh.visibility_index.append(boxes);
        }
        else
        {
            mesh.visibility_index.rebuild(std::move(boxes));
        }
        mesh.visible.reserve(mesh.instances.size());

        const auto transform_bytes = mesh.transforms.size() * sizeof(instance_data);
        if (!mesh.instance_buffer.handle ||
            mesh.instance_buffer.size < transform_bytes)
        {
            auto replacement = vk::buffer::create(
                std::max<vk::DeviceSize>(
                    static_cast<vk::DeviceSize>(transform_bytes),
                    mesh.instance_buffer.size * 2
                ),
                vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
            );
            vk::buffer::write(
                replacement, mesh.transforms.data(), transform_bytes
            );

            if (mesh.instance_buffer.handle)
            {
                vk::buffer::destroy(mesh.instance_buffer);
            }
            mesh.instance_buffer = replacement;
            bind_instances(mesh);
        }
        else if (first < mesh.transforms.size())
        {
            vk::buffer::write(
                mesh.instance_buffer, mesh.transforms.data() + first,
                (mesh.transforms.size() - first) * sizeof(instance_data),
                first * sizeof(instance_data)
            );
        }

        const auto visible_bytes = mesh.instances.size() * sizeof(std::uint32_t);
        if (!mesh.visible_buffer.handle || mesh.visible_buffer.size < visible_bytes)
        {
            auto replacement = vk::buffer::create(
                std::max<vk::DeviceSize>(
                    static_cast<vk::DeviceSize>(visible_bytes),
                    mesh.visible_buffer.size * 2
                ),
                vk::BufferUsageFlagBits::eVertexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
            );
            if (mesh.visible_buffer.handle)
            {
                vk::buffer::destroy(mesh.visible_buffer);
            }
            mesh.visible_buffer = replacement;
        }
    }

    void synchronize()
    {
        auto& data = get();
        const auto source = connection::snapshot();

        if (source == data.snapshot)
        {
            return;
        }

        // Scene batches are append-only; reuse immutable GPU meshes and expand
        // only the instance buffers/BVHs that gained objects.
        bool appended = data.snapshot && source &&
            source->meshes.size() == data.snapshot->meshes.size() &&
            source->instances.size() >= data.snapshot->instances.size();

        if (appended)
        {
            for (std::size_t i = 0; i < source->meshes.size(); ++i)
            {
                const auto& a = source->meshes[i];
                const auto& b = data.snapshot->meshes[i];
                if (a.id != b.id || a.vertices.size() != b.vertices.size() ||
                    a.indices.size() != b.indices.size())
                {
                    appended = false;
                    break;
                }
            }
        }

        if (appended)
        {
            const auto equal_pose = [](const ::scene::instance& a, const ::scene::instance& b)
            {
                return a.id == b.id && a.mesh_id == b.mesh_id &&
                    a.pose.position.x == b.pose.position.x &&
                    a.pose.position.y == b.pose.position.y &&
                    a.pose.position.z == b.pose.position.z &&
                    a.pose.scale.x == b.pose.scale.x &&
                    a.pose.scale.y == b.pose.scale.y &&
                    a.pose.scale.z == b.pose.scale.z &&
                    a.pose.rotation.x == b.pose.rotation.x &&
                    a.pose.rotation.y == b.pose.rotation.y &&
                    a.pose.rotation.z == b.pose.rotation.z &&
                    a.pose.rotation.w == b.pose.rotation.w;
            };
            appended = std::equal(
                data.snapshot->instances.begin(), data.snapshot->instances.end(),
                source->instances.begin(), equal_pose
            );
        }

        if (appended)
        {
            std::vector<std::size_t> previous_counts;
            previous_counts.reserve(data.meshes.size());
            for (const auto& mesh : data.meshes)
            {
                previous_counts.push_back(mesh.instances.size());
            }

            for (std::size_t i = data.snapshot->instances.size();
                 i < source->instances.size(); ++i)
            {
                const auto& instance = source->instances[i];
                for (std::size_t j = 0; j < data.meshes.size(); ++j)
                {
                    if (data.meshes[j].id == instance.mesh_id)
                    {
                        append_instance(data.meshes[j], instance);
                        break;
                    }
                }
            }
            for (std::size_t i = 0; i < data.meshes.size(); ++i)
            {
                if (data.meshes[i].instances.size() != previous_counts[i])
                {
                    refresh_instances(data.meshes[i], previous_counts[i]);
                }
            }
            data.snapshot = source;
            return;
        }

        std::vector<gpu_mesh> meshes;
        vk::DescriptorPool new_pool{};

        if (source)
        {
            meshes.reserve(source->meshes.size());

            for (const auto& mesh : source->meshes)
            {
                gpu_mesh result;
                result.id = mesh.id;
                result.index_count =
                    static_cast<std::uint32_t>(mesh.indices.size());

                try
                {
                    const auto vertex_bytes =
                        mesh.vertices.size() * sizeof(::scene::vertex);

                    const auto index_bytes =
                        mesh.indices.size() * sizeof(std::uint32_t);

                    result.vertices = upload::static_buffer(
                        mesh.vertices.data(),
                        vertex_bytes,
                        vk::BufferUsageFlagBits::eVertexBuffer
                    );

                    result.indices = upload::static_buffer(
                        mesh.indices.data(),
                        index_bytes,
                        vk::BufferUsageFlagBits::eIndexBuffer
                    );

                    if (!mesh.vertices.empty())
                    {
                        vec3 low = mesh.vertices.front().position;
                        vec3 high = low;
                        for (const auto& vertex : mesh.vertices)
                        {
                            const auto p = vertex.position;
                            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                        }
                        result.center = (low + high) * 0.5f;
                        result.radius = length((high - low) * 0.5f);
                    }

                    meshes.push_back(result);
                }
                catch (...)
                {
                    if (result.vertices.handle)
                    {
                        vk::buffer::destroy(result.vertices);
                    }

                    if (result.indices.handle)
                    {
                        vk::buffer::destroy(result.indices);
                    }

                    destroy_meshes(meshes);
                    throw;
                }
            }
        }

        try
        {
            if (source)
            {
                for (const auto& instance : source->instances)
                {
                    const auto it = std::find_if(meshes.begin(), meshes.end(),
                        [&](const gpu_mesh& mesh) { return mesh.id == instance.mesh_id; });
                    if (it == meshes.end() || !it->index_count)
                    {
                        continue;
                    }

                    append_instance(*it, instance);
                }

                // Each mesh owns a descriptor referencing its persistent
                // transform storage; no per-frame matrix upload is required.
                vk::DescriptorPoolSize pool_size{};
                pool_size.type = vk::DescriptorType::eStorageBuffer;
                pool_size.descriptorCount =
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, meshes.size()));

                vk::DescriptorPoolCreateInfo pool_info{};
                pool_info.maxSets = pool_size.descriptorCount;
                pool_info.poolSizeCount = 1;
                pool_info.pPoolSizes = &pool_size;
                const auto device = vk::device::get()->handle;
                new_pool = device.createDescriptorPool(pool_info);

                for (auto& mesh : meshes)
                {
                    if (mesh.instances.empty())
                    {
                        continue;
                    }

                    refresh_instances(mesh);

                    vk::DescriptorSetAllocateInfo allocate{};
                    allocate.descriptorPool = new_pool;
                    allocate.descriptorSetCount = 1;
                    allocate.pSetLayouts = &data.instance_layout;
                    mesh.instance_set = device.allocateDescriptorSets(allocate).front();
                    bind_instances(mesh);
                }
            }
        }
        catch (...)
        {
            destroy_meshes(meshes);
            if (new_pool)
            {
                vk::device::get()->handle.destroyDescriptorPool(new_pool);
            }
            throw;
        }

        destroy_meshes(data.meshes);
        if (data.instance_pool)
        {
            vk::device::get()->handle.destroyDescriptorPool(data.instance_pool);
        }
        data.instance_pool = new_pool;
        data.meshes = std::move(meshes);
        data.snapshot = source;
    }

    void draw(vk::CommandBuffer command, std::uint32_t image)
    {
        using clock = std::chrono::steady_clock;

        auto& data = get();
        data.timings = {};
        const auto extent = vk::swapchain::get()->extent;

        std::array<vk::ClearValue, 2> clear{};
        clear[0].color = vk::ClearColorValue(
            std::array<float, 4>{0.035f, 0.045f, 0.065f, 1.0f}
        );
        clear[1].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

        vk::RenderPassBeginInfo begin{};
        begin.renderPass = data.render_pass;
        begin.framebuffer = data.framebuffers.at(image);
        begin.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        begin.clearValueCount = clear.size();
        begin.pClearValues = clear.data();

        command.beginRenderPass(begin, vk::SubpassContents::eInline);

        command.bindPipeline(
            vk::PipelineBindPoint::eGraphics,
            data.pipeline
        );

        command.setViewport(0, vk::Viewport{
            0.0f,
            0.0f,
            float(extent.width),
            float(extent.height),
            0.0f,
            1.0f
        });

        command.setScissor(0, vk::Rect2D{{0, 0}, extent});

        if (data.snapshot)
        {
            const auto view_projection =
                camera::projection(
                    float(extent.width) / float(extent.height)
                ) * camera::view();

            const auto planes = frustum(view_projection);
            std::array<spatial::plane, 6> visibility_planes{};

            for (std::size_t i = 0; i < visibility_planes.size(); ++i)
            {
                visibility_planes[i] = {planes[i].normal, planes[i].offset};
            }

            command.pushConstants(
                data.layout, vk::ShaderStageFlagBits::eVertex,
                0, sizeof(view_projection), &view_projection
            );

            for (auto& mesh : data.meshes)
            {
                const auto start = clock::now();
                mesh.visible.clear();

                const auto stats = mesh.visibility_index.query_frustum_fast(
                    visibility_planes,
                    [&](std::uint32_t id)
                    {
                        mesh.visible.push_back(id);
                    }
                );

                data.timings.cull_ms += std::chrono::duration<double, std::milli>(
                    clock::now() - start).count();
                data.timings.tested_nodes += stats.tested_nodes;
                data.timings.accepted_groups += stats.accepted_groups;
                data.timings.visible_instances += mesh.visible.size();

                if (mesh.visible.empty())
                {
                    continue;
                }

                const auto upload_begin = clock::now();
                vk::buffer::write(
                    mesh.visible_buffer, mesh.visible.data(),
                    mesh.visible.size() * sizeof(std::uint32_t)
                );
                data.timings.index_upload_ms += std::chrono::duration<double, std::milli>(
                    clock::now() - upload_begin).count();

                const auto record_begin = clock::now();
                const std::array<vk::Buffer, 2> buffers{
                    mesh.vertices.handle, mesh.visible_buffer.handle
                };
                const std::array<vk::DeviceSize, 2> offsets{0, 0};
                command.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    data.layout, 0, {mesh.instance_set}, {}
                );
                command.bindVertexBuffers(0, buffers, offsets);
                command.bindIndexBuffer(mesh.indices.handle, 0, vk::IndexType::eUint32);
                command.drawIndexed(
                    mesh.index_count, static_cast<std::uint32_t>(mesh.visible.size()), 0, 0, 0
                );
                data.timings.commands_ms += std::chrono::duration<double, std::milli>(
                    clock::now() - record_begin).count();
                ++data.timings.draw_calls;
            }
        }

        command.endRenderPass();
    }

    void shutdown()
    {
        auto& data = get();

        destroy_meshes(data.meshes);
        data.snapshot.reset();
        if (data.instance_pool)
        {
            vk::device::get()->handle.destroyDescriptorPool(data.instance_pool);
            data.instance_pool = nullptr;
        }

        release_frame_resources();

        if (data.instance_layout)
        {
            vk::device::get()->handle.destroyDescriptorSetLayout(data.instance_layout);
            data.instance_layout = nullptr;
        }
    }
} // namespace clt::scene_renderer
