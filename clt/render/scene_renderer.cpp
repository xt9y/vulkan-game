#include "scene_renderer.hpp"

#include "clt/network/connection.hpp"
#include "clt/render/upload.hpp"
#include "clt/view/camera.hpp"
#include "srv/shared/scene/scene.hpp"

#include <algorithm>
#include <array>
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

        struct gpu_mesh {
            std::uint32_t id = 0;
            vk::buffer::resource vertices;
            vk::buffer::resource indices;
            vk::buffer::resource instance_buffer;
            vk::buffer::resource bounds_buffer;

            struct per_image {
                vk::buffer::resource visible;
                vk::buffer::resource indirect;
                vk::DescriptorSet set{};
            };
            std::vector<per_image> frames;
            std::vector<std::array<float, 4>> bounds;
            std::vector<instance_data> transforms;
            vec3 center;
            float radius = 0;
            std::uint32_t index_count = 0;
        };

        static_assert(sizeof(instance_data) == 128);
        static_assert(sizeof(::scene::vertex) == 32);

        struct cull_parameters {
            std::array<vec4, 6> planes{};
            std::uint32_t count = 0;
            std::uint32_t index_count = 0;
        };
        static_assert(sizeof(cull_parameters) <= 128);

        struct state {
            std::shared_ptr<const ::scene::state> snapshot;
            std::vector<gpu_mesh> meshes;
            vk::DescriptorSetLayout instance_layout{};
            vk::DescriptorPool instance_pool{};
            vk::PipelineLayout cull_layout{};
            vk::Pipeline cull_pipeline{};
            std::array<vec4, 6> last_gpu_planes{};
            bool gpu_planes_valid = false;
            std::uint32_t last_image = 0;

            vk::RenderPass render_pass{};
            vk::PipelineLayout layout{};
            vk::Pipeline pipeline{};

            struct target {
                std::array<vk::image::resource, 3> color;
                std::array<vk::ImageView, 3> views{};
                vk::image::resource depth;
                vk::ImageView depth_view{};
                vk::Framebuffer framebuffer{};
            };

            std::vector<target> targets;

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
                if (mesh.bounds_buffer.handle)
                {
                    vk::buffer::destroy(mesh.bounds_buffer);
                }
                for (auto& frame : mesh.frames)
                {
                    if (frame.visible.handle) vk::buffer::destroy(frame.visible);
                    if (frame.indirect.handle) vk::buffer::destroy(frame.indirect);
                }
            }

            meshes.clear();
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

        void create_pipeline()
        {
            auto& data = get();

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

                std::array<vk::PipelineColorBlendAttachmentState, 3> attachments{
                    blend, blend, blend
                };
                vk::PipelineColorBlendStateCreateInfo blending{};
                blending.attachmentCount = attachments.size();
                blending.pAttachments = attachments.data();

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


        constexpr std::array<vk::Format, 3> color_formats{
            vk::Format::eR8G8B8A8Unorm,
            vk::Format::eR8G8B8A8Unorm,
            vk::Format::eR8G8B8A8Unorm
        };

        vk::Format depth_format()
        {
            const auto gpu = vk::physical_device::get()->handle;
            for (auto format : {vk::Format::eD32Sfloat, vk::Format::eD16Unorm})
            {
                const auto props = gpu.getFormatProperties(format);
                const auto needed = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                    vk::FormatFeatureFlagBits::eSampledImage;
                if ((props.optimalTilingFeatures & needed) == needed)
                {
                    return format;
                }
            }
            throw std::runtime_error("No sampled depth attachment format available");
        }

        void create_render_pass()
        {
            auto& data = get();
            std::array<vk::AttachmentDescription, 4> attachments{};
            std::array<vk::AttachmentReference, 3> colors{};

            for (std::uint32_t i = 0; i < colors.size(); ++i)
            {
                attachments[i].format = color_formats[i];
                attachments[i].samples = vk::SampleCountFlagBits::e1;
                attachments[i].loadOp = vk::AttachmentLoadOp::eClear;
                attachments[i].storeOp = vk::AttachmentStoreOp::eStore;
                attachments[i].initialLayout = vk::ImageLayout::eUndefined;
                attachments[i].finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                colors[i] = {i, vk::ImageLayout::eColorAttachmentOptimal};
            }

            attachments[3].format = depth_format();
            attachments[3].samples = vk::SampleCountFlagBits::e1;
            attachments[3].loadOp = vk::AttachmentLoadOp::eClear;
            attachments[3].storeOp = vk::AttachmentStoreOp::eStore;
            attachments[3].initialLayout = vk::ImageLayout::eUndefined;
            attachments[3].finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;

            vk::AttachmentReference depth{3, vk::ImageLayout::eDepthStencilAttachmentOptimal};
            vk::SubpassDescription subpass{};
            subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
            subpass.colorAttachmentCount = colors.size();
            subpass.pColorAttachments = colors.data();
            subpass.pDepthStencilAttachment = &depth;

            std::array<vk::SubpassDependency, 2> barriers{};
            barriers[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            barriers[0].dstSubpass = 0;
            barriers[0].srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
            barriers[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eEarlyFragmentTests;
            barriers[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            barriers[1].srcSubpass = 0;
            barriers[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            barriers[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eLateFragmentTests;
            barriers[1].dstStageMask = vk::PipelineStageFlagBits::eComputeShader;
            barriers[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;

            vk::RenderPassCreateInfo info{};
            info.attachmentCount = attachments.size();
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = barriers.size();
            info.pDependencies = barriers.data();
            data.render_pass = vk::render_pass::create(info);
        }

        void create_target()
        {
            auto& data = get();
            const auto extent = vk::swapchain::get()->extent;
            state::target target{};
            try
            {
                for (std::size_t i = 0; i < color_formats.size(); ++i)
                {
                    vk::ImageCreateInfo image{};
                    image.imageType = vk::ImageType::e2D;
                    image.format = color_formats[i];
                    image.extent = {extent.width, extent.height, 1};
                    image.mipLevels = 1;
                    image.arrayLayers = 1;
                    image.samples = vk::SampleCountFlagBits::e1;
                    image.tiling = vk::ImageTiling::eOptimal;
                    image.usage = vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled;
                    target.color[i] = vk::image::create(
                        image, vk::MemoryPropertyFlagBits::eDeviceLocal
                    );

                    vk::ImageViewCreateInfo view{};
                    view.image = target.color[i].handle;
                    view.viewType = vk::ImageViewType::e2D;
                    view.format = image.format;
                    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                    target.views[i] = vk::image_view::create(view);
                }

                vk::ImageCreateInfo depth{};
                depth.imageType = vk::ImageType::e2D;
                depth.format = depth_format();
                depth.extent = {extent.width, extent.height, 1};
                depth.mipLevels = 1;
                depth.arrayLayers = 1;
                depth.samples = vk::SampleCountFlagBits::e1;
                depth.tiling = vk::ImageTiling::eOptimal;
                depth.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                    vk::ImageUsageFlagBits::eSampled;
                target.depth = vk::image::create(depth, vk::MemoryPropertyFlagBits::eDeviceLocal);

                vk::ImageViewCreateInfo depth_view{};
                depth_view.image = target.depth.handle;
                depth_view.viewType = vk::ImageViewType::e2D;
                depth_view.format = depth.format;
                depth_view.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
                target.depth_view = vk::image_view::create(depth_view);

                const std::array<vk::ImageView, 4> attachments{
                    target.views[0], target.views[1], target.views[2], target.depth_view
                };
                vk::FramebufferCreateInfo framebuffer{};
                framebuffer.renderPass = data.render_pass;
                framebuffer.attachmentCount = attachments.size();
                framebuffer.pAttachments = attachments.data();
                framebuffer.width = extent.width;
                framebuffer.height = extent.height;
                framebuffer.layers = 1;
                target.framebuffer = vk::framebuffer::create(framebuffer);
                data.targets.push_back(std::move(target));
            }
            catch (...)
            {
                if (target.framebuffer) vk::framebuffer::destroy(target.framebuffer);
                if (target.depth_view) vk::image_view::destroy(target.depth_view);
                if (target.depth.handle) vk::image::destroy(target.depth);
                for (std::size_t i = 0; i < target.color.size(); ++i)
                {
                    if (target.views[i]) vk::image_view::destroy(target.views[i]);
                    if (target.color[i].handle) vk::image::destroy(target.color[i]);
                }
                throw;
            }
        }
    } // namespace

    void init(const char* vertex_path, const char* fragment_path)
    {
        if (!vertex_path || !fragment_path || !*vertex_path || !*fragment_path)
        {
            throw std::invalid_argument("Scene shader paths must not be empty");
        }

        auto& data = get();
        data.shaders = {vertex_path, fragment_path};

        std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
            if (i == 0)
            {
                bindings[i].stageFlags |= vk::ShaderStageFlagBits::eVertex;
            }
        }

        const auto device = vk::device::get()->handle;
        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        data.instance_layout = device.createDescriptorSetLayout(info);


        vk::PushConstantRange range{};
        range.stageFlags = vk::ShaderStageFlagBits::eCompute;
        range.offset = 0;
        range.size = sizeof(cull_parameters);

        vk::PipelineLayoutCreateInfo layout{};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &data.instance_layout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &range;
        data.cull_layout = device.createPipelineLayout(layout);

        const auto code = vk::shader::read("shd/cull.comp.spv");
        auto module = vk::shader::create(code);
        try
        {
            vk::ComputePipelineCreateInfo pipeline{};
            pipeline.stage.stage = vk::ShaderStageFlagBits::eCompute;
            pipeline.stage.module = module;
            pipeline.stage.pName = "main";
            pipeline.layout = data.cull_layout;
            data.cull_pipeline = vk::pipeline::compute(pipeline);
        }
        catch (...)
        {
            vk::shader::destroy(module);
            throw;
        }
        vk::shader::destroy(module);
    }

    culling_readback culling_data()
    {
        auto& data = get();
        culling_readback result;
        result.planes = data.last_gpu_planes;
        result.valid = data.gpu_planes_valid;
        result.meshes.reserve(data.meshes.size());

        for (auto& mesh : data.meshes)
        {
            if (!mesh.transforms.empty() && data.last_image < mesh.frames.size())
            {
                auto& frame = mesh.frames[data.last_image];
                result.meshes.push_back({
                    &mesh.bounds, &frame.visible,
                    &frame.indirect, mesh.index_count
                });
            }
        }
        return result;
    }


    gbuffer outputs(std::uint32_t image)
    {
        const auto& target = get().targets.at(image);
        return {{
            target.views[0], target.views[1], target.views[2], target.depth_view
        }, vk::swapchain::get()->extent};
    }

    void release_frame_resources()
    {
        auto& data = get();
        vk::pipeline::destroy(data.pipeline);
        data.pipeline = nullptr;
        vk::pipeline_layout::destroy(data.layout);
        data.layout = nullptr;

        for (auto& target : data.targets)
        {
            if (target.framebuffer) vk::framebuffer::destroy(target.framebuffer);
            if (target.depth_view) vk::image_view::destroy(target.depth_view);
            if (target.depth.handle) vk::image::destroy(target.depth);
            for (std::size_t i = 0; i < target.color.size(); ++i)
            {
                if (target.views[i]) vk::image_view::destroy(target.views[i]);
                if (target.color[i].handle) vk::image::destroy(target.color[i]);
            }
        }
        data.targets.clear();
        vk::render_pass::destroy(data.render_pass);
        data.render_pass = nullptr;
    }

    void create_frame_resources()
    {
        try
        {
            create_render_pass();
            for (std::size_t i = 0; i < vk::swapchain::get()->images.size(); ++i)
            {
                create_target();
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
        mesh.transforms.push_back({
            trs(pose.position, pose.rotation, pose.scale),
            rotation(pose.rotation) * scale(inverse)
        });
        const auto position = pose.position + rotate(pose.rotation, center);
        mesh.bounds.push_back({
            position.x, position.y, position.z, mesh.radius * max_scale
        });
    }

    void bind_instances(gpu_mesh& mesh)
    {
        if (!mesh.instance_buffer.handle || !mesh.bounds_buffer.handle)
        {
            return;
        }

        for (auto& frame : mesh.frames)
        {
            if (!frame.set || !frame.visible.handle || !frame.indirect.handle)
            {
                continue;
            }
            const std::array<vk::Buffer, 4> buffers{
                mesh.instance_buffer.handle,
                mesh.bounds_buffer.handle,
                frame.visible.handle,
                frame.indirect.handle
            };
            std::array<vk::DescriptorBufferInfo, 4> infos{};
            std::array<vk::WriteDescriptorSet, 4> writes{};

            for (std::uint32_t i = 0; i < buffers.size(); ++i)
            {
                infos[i].buffer = buffers[i];
                infos[i].offset = 0;
                infos[i].range = VK_WHOLE_SIZE;
                writes[i].dstSet = frame.set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                writes[i].pBufferInfo = &infos[i];
            }
            vk::device::get()->handle.updateDescriptorSets(writes, {});
        }
    }

    void refresh_instances(gpu_mesh& mesh, std::size_t first = 0)
    {
        if (mesh.transforms.empty())
        {
            return;
        }

        upload::batch transfers;
        const auto transform_bytes = mesh.transforms.size() * sizeof(instance_data);
        if (!mesh.instance_buffer.handle || mesh.instance_buffer.size < transform_bytes)
        {
            auto replacement = transfers.add(
                mesh.transforms.data(), transform_bytes,
                vk::BufferUsageFlagBits::eStorageBuffer,
                std::max<vk::DeviceSize>(transform_bytes, mesh.instance_buffer.size * 2)
            );
            if (mesh.instance_buffer.handle) vk::buffer::destroy(mesh.instance_buffer);
            mesh.instance_buffer = replacement;
        }
        else if (first < mesh.transforms.size())
        {
            transfers.write(
                mesh.instance_buffer, mesh.transforms.data() + first,
                (mesh.transforms.size() - first) * sizeof(instance_data),
                first * sizeof(instance_data)
            );
        }

        const auto visible_bytes = mesh.bounds.size() * sizeof(std::uint32_t);
        const auto count = vk::swapchain::get()->images.size();
        mesh.frames.resize(count);
        for (auto& frame : mesh.frames)
        {
            if (!frame.visible.handle || frame.visible.size < visible_bytes)
            {
                auto replacement = vk::buffer::create(
                    std::max<vk::DeviceSize>(visible_bytes, frame.visible.size * 2),
                    vk::BufferUsageFlagBits::eVertexBuffer |
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                if (frame.visible.handle) vk::buffer::destroy(frame.visible);
                frame.visible = replacement;
            }

            if (!frame.indirect.handle)
            {
                frame.indirect = vk::buffer::create(
                    sizeof(vk::DrawIndexedIndirectCommand),
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst |
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
            }
        }

        const auto size = mesh.bounds.size() * sizeof(mesh.bounds.front());
        if (!mesh.bounds_buffer.handle || mesh.bounds_buffer.size < size)
        {
            auto replacement = transfers.add(
                mesh.bounds.data(), size, vk::BufferUsageFlagBits::eStorageBuffer,
                std::max<vk::DeviceSize>(size, mesh.bounds_buffer.size * 2)
            );
            if (mesh.bounds_buffer.handle) vk::buffer::destroy(mesh.bounds_buffer);
            mesh.bounds_buffer = replacement;
        }
        else if (first < mesh.bounds.size())
        {
            transfers.write(
                mesh.bounds_buffer, mesh.bounds.data() + first,
                (mesh.bounds.size() - first) * sizeof(mesh.bounds.front()),
                first * sizeof(mesh.bounds.front())
            );
        }

        transfers.finish();
        bind_instances(mesh);
    }

    bool needs_sync()
    {
        return connection::snapshot() != get().snapshot;
    }

    void reset_scene()
    {
        auto& data = get();
        destroy_meshes(data.meshes);
        if (data.instance_pool)
        {
            vk::device::get()->handle.destroyDescriptorPool(data.instance_pool);
            data.instance_pool = nullptr;
        }
        data.snapshot.reset();
        data.gpu_planes_valid = false;
    }

    void synchronize()
    {
        auto& data = get();
        const auto source = connection::snapshot();

        if (source == data.snapshot)
        {
            return;
        }

        // Scene batches are append-only; reuse immutable GPU meshes and grow
        // only the buffers containing newly generated instances.
        bool appended = data.snapshot && source &&
            source->meshes.size() == data.snapshot->meshes.size() &&
            source->instances.size() >= data.snapshot->instances.size();

        if (appended)
        {
            for (std::size_t i = 0; i < source->meshes.size(); ++i)
            {
                const auto& a = source->meshes[i];
                const auto& b = data.snapshot->meshes[i];
                if (!::scene::same_mesh(a, b))
                {
                    appended = false;
                    break;
                }
            }
        }

        if (appended)
        {
            appended = std::equal(
                data.snapshot->instances.begin(), data.snapshot->instances.end(),
                source->instances.begin(), ::scene::same_instance
            );
        }

        if (appended)
        {
            std::vector<std::size_t> previous_counts;
            previous_counts.reserve(data.meshes.size());
            for (const auto& mesh : data.meshes)
            {
                previous_counts.push_back(mesh.transforms.size());
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
                if (data.meshes[i].transforms.size() != previous_counts[i])
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

            upload::batch transfers;
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

                    result.vertices = transfers.add(
                        mesh.vertices.data(),
                        vertex_bytes,
                        vk::BufferUsageFlagBits::eVertexBuffer
                    );

                    result.indices = transfers.add(
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
            transfers.finish();
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
                pool_size.descriptorCount = static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, meshes.size()) *
                    std::max<std::size_t>(1, vk::swapchain::get()->images.size()) * 4
                );

                vk::DescriptorPoolCreateInfo pool_info{};
                pool_info.maxSets = static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, meshes.size()) *
                    std::max<std::size_t>(1, vk::swapchain::get()->images.size())
                );
                pool_info.poolSizeCount = 1;
                pool_info.pPoolSizes = &pool_size;
                const auto device = vk::device::get()->handle;
                new_pool = device.createDescriptorPool(pool_info);

                for (auto& mesh : meshes)
                {
                    if (mesh.transforms.empty())
                    {
                        continue;
                    }

                    refresh_instances(mesh);

                    for (auto& frame : mesh.frames)
                    {
                        vk::DescriptorSetAllocateInfo allocate{};
                        allocate.descriptorPool = new_pool;
                        allocate.descriptorSetCount = 1;
                        allocate.pSetLayouts = &data.instance_layout;
                        frame.set = device.allocateDescriptorSets(allocate).front();
                    }
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
        auto& data = get();
        const auto extent = vk::swapchain::get()->extent;
        data.last_image = image;
        const auto view_projection = camera::projection(
            float(extent.width) / float(extent.height)
        ) * camera::view();

        std::array<vk::ClearValue, 4> clear{};
        for (std::size_t i = 0; i < 3; ++i)
        {
            clear[i].color = vk::ClearColorValue(
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
            );
        }
        clear[3].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

        vk::RenderPassBeginInfo begin{};
        begin.renderPass = data.render_pass;
        begin.framebuffer = data.targets.at(image).framebuffer;
        begin.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        begin.clearValueCount = clear.size();
        begin.pClearValues = clear.data();

        // Compute visibility and compact instance IDs for indirect graphics draws.
        if (data.snapshot)
        {
            const auto camera_planes = frustum(view_projection);
            cull_parameters params;
            for (std::size_t i = 0; i < params.planes.size(); ++i)
            {
                const auto& plane = camera_planes[i];
                const float inverse = 1.0f / std::max(plane.magnitude, 1e-6f);
                params.planes[i] = {
                    plane.normal.x * inverse,
                    plane.normal.y * inverse,
                    plane.normal.z * inverse,
                    plane.offset * inverse
                };
            }

            data.last_gpu_planes = params.planes;
            data.gpu_planes_valid = true;

            for (const auto& mesh : data.meshes)
            {
                if (!mesh.transforms.empty())
                {
                    command.fillBuffer(
                        mesh.frames.at(image).indirect.handle,
                        offsetof(vk::DrawIndexedIndirectCommand, instanceCount),
                        sizeof(std::uint32_t), 0
                    );
                }
            }

            vk::MemoryBarrier transfer{};
            transfer.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            transfer.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                vk::AccessFlagBits::eShaderWrite;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eComputeShader,
                {}, {transfer}, {}, {}
            );

            command.bindPipeline(vk::PipelineBindPoint::eCompute, data.cull_pipeline);
            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty())
                {
                    continue;
                }

                params.count = static_cast<std::uint32_t>(mesh.transforms.size());
                params.index_count = mesh.index_count;
                command.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute,
                    data.cull_layout, 0, {mesh.frames.at(image).set}, {}
                );
                command.pushConstants(
                    data.cull_layout, vk::ShaderStageFlagBits::eCompute,
                    0, sizeof(params), &params
                );
                command.dispatch((params.count + 127u) / 128u, 1, 1);
            }

            vk::MemoryBarrier finished{};
            finished.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            finished.dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead |
                vk::AccessFlagBits::eIndirectCommandRead;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eVertexInput |
                vk::PipelineStageFlagBits::eDrawIndirect,
                {}, {finished}, {}, {}
            );

        }

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
            command.pushConstants(
                data.layout, vk::ShaderStageFlagBits::eVertex,
                0, sizeof(view_projection), &view_projection
            );

            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty())
                {
                    continue;
                }

                const std::array<vk::Buffer, 2> buffers{
                    mesh.vertices.handle, mesh.frames.at(image).visible.handle
                };
                const std::array<vk::DeviceSize, 2> offsets{0, 0};
                command.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    data.layout, 0, {mesh.frames.at(image).set}, {}
                );
                command.bindVertexBuffers(0, buffers, offsets);
                command.bindIndexBuffer(
                    mesh.indices.handle, 0, vk::IndexType::eUint32
                );
                command.drawIndexedIndirect(
                    mesh.frames.at(image).indirect.handle, 0, 1,
                    sizeof(vk::DrawIndexedIndirectCommand)
                );
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

        if (data.cull_pipeline)
        {
            vk::pipeline::destroy(data.cull_pipeline);
            data.cull_pipeline = nullptr;
        }
        if (data.cull_layout)
        {
            vk::device::get()->handle.destroyPipelineLayout(data.cull_layout);
            data.cull_layout = nullptr;
        }
        if (data.instance_layout)
        {
            vk::device::get()->handle.destroyDescriptorSetLayout(data.instance_layout);
            data.instance_layout = nullptr;
        }
    }
} // namespace clt::scene_renderer
