#include "scene_renderer.hpp"

#include "clt/animation/animation.hpp"
#include "clt/network/connection.hpp"
#include "clt/render/texture.hpp"
#include "clt/render/upload.hpp"
#include "clt/render/vat_gpu.hpp"
#include "clt/view/camera.hpp"
#include "clt/weapon/weapon.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/limits.hpp"
#include "srv/shared/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

namespace clt::scene_renderer
{
    namespace
    {
        using namespace vk::util::math;

        struct instance_data {
            mat4 model;
            mat4 normal;
        };

        struct draw_parameters {
            mat4 view_projection;
            std::array<float, 4> base_color;
            std::array<float, 4> surface;
            std::array<float, 4> emissive;
            std::uint32_t joint_count = 0;
            std::uint32_t wound_enabled = 0;
        };
        static_assert(sizeof(draw_parameters) <= 128);

        constexpr std::size_t wound_atlas_resolution = 128;
        constexpr std::size_t wound_atlas_words =
            wound_atlas_resolution * wound_atlas_resolution / 4;

        struct gpu_mesh {
            std::uint32_t id = 0;
            ::scene::material surface;
            std::uint32_t skin_id = 0;
            std::uint32_t joint_count = 0;
            bool has_wounds = false;
            vk::buffer::resource vertices;
            vk::buffer::resource indices;
            vk::buffer::resource instance_buffer;
            vk::buffer::resource bounds_buffer;

            struct per_image {
                vk::buffer::resource visible;
                vk::buffer::resource indirect;
                vk::buffer::resource skin_palette;
                vk::buffer::resource local_instances;
                vk::buffer::resource vat_data;
                vk::buffer::resource wounds;
                vk::DescriptorSet set{};
            };
            std::vector<per_image> frames;
            std::vector<std::array<float, 4>> bounds;
            std::vector<instance_data> transforms;
            std::vector<::scene::instance> instances;
            vec3 center;
            float radius = 0;
            std::uint32_t index_count = 0;
        };

        static_assert(sizeof(instance_data) == 128);
        static_assert(sizeof(::scene::vertex) == 64);

        struct cull_parameters {
            std::array<vec4, 6> planes{};
            std::uint32_t count = 0;
            std::uint32_t index_count = 0;
        };
        static_assert(sizeof(cull_parameters) <= 128);

        struct state {
            std::shared_ptr<const ::scene::state> snapshot;
            std::vector<gpu_mesh> meshes;
            std::vector<gpu_mesh> local_meshes;
            vk::DescriptorPool local_pool{};
            vk::buffer::resource vat_fallback;
            vk::buffer::resource wound_fallback;
            vk::DescriptorSetLayout instance_layout{};
            vk::DescriptorPool instance_pool{};
            vk::PipelineLayout cull_layout{};
            vk::Pipeline cull_pipeline{};
            std::array<vec4, 6> last_gpu_planes{};
            bool gpu_planes_valid = false;
            std::uint32_t last_image = 0;
            mat4 last_world_view_projection = mat4::identity();
            std::uint32_t last_animation_tick = UINT32_MAX;
            std::chrono::steady_clock::time_point animation_received{};

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
                    if (frame.skin_palette.handle) vk::buffer::destroy(frame.skin_palette);
                    if (frame.local_instances.handle)
                    {
                        vk::buffer::destroy(frame.local_instances);
                    }
                    if (frame.vat_data.handle)
                    {
                        vk::buffer::destroy(frame.vat_data);
                    }
                    if (frame.wounds.handle) vk::buffer::destroy(frame.wounds);
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
                range.stageFlags = vk::ShaderStageFlagBits::eVertex |
                    vk::ShaderStageFlagBits::eFragment;
                range.offset = 0;
                range.size = sizeof(draw_parameters);

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

                std::array<vk::VertexInputAttributeDescription, 6> attributes{};
                attributes[0] = vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32B32Sfloat,
                                 offsetof(::scene::vertex, position)};
                attributes[1] = vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32B32Sfloat,
                                 offsetof(::scene::vertex, normal)};
                attributes[2] = vk::VertexInputAttributeDescription{
                    2, 0, vk::Format::eR32G32Sfloat, offsetof(::scene::vertex, u)
                };
                attributes[3] = vk::VertexInputAttributeDescription{
                    3, 1, vk::Format::eR32Uint, 0
                };
                attributes[4] = vk::VertexInputAttributeDescription{
                    4, 0, vk::Format::eR32G32B32A32Uint,
                    offsetof(::scene::vertex, joints)
                };
                attributes[5] = vk::VertexInputAttributeDescription{
                    5, 0, vk::Format::eR32G32B32A32Sfloat,
                    offsetof(::scene::vertex, weights)
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


        std::array<vk::Format, 3> color_formats()
        {
            const auto precision = game::options::get()->graphics.gbuffer;
            const auto format = precision == game::options::gbuffer_precision::rgba16f
                ? vk::Format::eR16G16B16A16Sfloat : vk::Format::eR8G8B8A8Unorm;
            return {format, format, format};
        }

        vk::Format depth_format()
        {
            const auto precision = game::options::get()->graphics.depth;
            const auto format = precision == game::options::depth_precision::d32
                ? vk::Format::eD32Sfloat : vk::Format::eD16Unorm;
            const auto properties = vk::physical_device::get()->handle
                .getFormatProperties(format);
            const auto required = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                vk::FormatFeatureFlagBits::eSampledImage;
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("Configured depth format unsupported");
            return format;
        }

        void create_render_pass()
        {
            auto& data = get();
            std::array<vk::AttachmentDescription, 4> attachments{};
            std::array<vk::AttachmentReference, 3> colors{};

            for (std::uint32_t i = 0; i < colors.size(); ++i)
            {
                attachments[i].format = color_formats()[i];
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
                vk::PipelineStageFlagBits::eEarlyFragmentTests |
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
                for (std::size_t i = 0; i < color_formats().size(); ++i)
                {
                    const auto features = vk::physical_device::get()->handle
                        .getFormatProperties(color_formats()[i]).optimalTilingFeatures;
                    const auto needed = vk::FormatFeatureFlagBits::eColorAttachment |
                        vk::FormatFeatureFlagBits::eSampledImage;
                    if ((features & needed) != needed)
                    {
                        throw std::runtime_error("Required G-buffer format unsupported");
                    }

                    vk::ImageCreateInfo image{};
                    image.imageType = vk::ImageType::e2D;
                    image.format = color_formats()[i];
                    image.extent = vk::Extent3D{extent.width, extent.height, 1};
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
                depth.extent = vk::Extent3D{extent.width, extent.height, 1};
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

        std::array<vk::DescriptorSetLayoutBinding, 15> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = (i < 4 || i == 9 || i == 13 || i == 14) ?
                vk::DescriptorType::eStorageBuffer :
                vk::DescriptorType::eCombinedImageSampler;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = i < 4 ?
                vk::ShaderStageFlags(vk::ShaderStageFlagBits::eCompute) :
                i == 9 || (i >= 10 && i <= 13) ?
                    vk::ShaderStageFlags(vk::ShaderStageFlagBits::eVertex) :
                    vk::ShaderStageFlags(vk::ShaderStageFlagBits::eFragment);
            if (i == 0)
                bindings[i].stageFlags |= vk::ShaderStageFlagBits::eVertex;
        }

        const auto device = vk::device::get()->handle;
        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        data.instance_layout = device.createDescriptorSetLayout(info);
        texture::init();
        weapon::initialize();
        data.vat_fallback = vk::buffer::create(
            sizeof(vat_gpu::parameters_t), vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent
        );
        const vat_gpu::parameters_t empty_vat{};
        vk::buffer::write(data.vat_fallback, &empty_vat, sizeof(empty_vat));
        data.wound_fallback = vk::buffer::create(
            sizeof(std::uint32_t), vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
        );
        const std::uint32_t empty_wounds = 0;
        vk::buffer::write(data.wound_fallback, &empty_wounds, sizeof(empty_wounds));
        // Blood effects now use attached decals and UV wounds; no unused
        // VAT textures are uploaded during normal renderer startup.
        texture::pin(weapon::assets());
        texture::synchronize(::scene::state{});

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

        const auto code = vk::shader::read(game::options::get()->graphics.culling_shader.c_str());
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


    mat4 world_view_projection()
    {
        return get().last_world_view_projection;
    }

    gbuffer outputs(std::uint32_t image)
    {
        const auto& target = get().targets.at(image);
        return {{
            target.views[0], target.views[1], target.views[2], target.depth_view
        }, vk::swapchain::get()->extent};
    }

    void release_local_frames()
    {
        auto& data = get();
        for (auto& mesh : data.local_meshes)
        {
            for (auto& frame : mesh.frames)
            {
                if (frame.local_instances.handle)
                {
                    vk::buffer::destroy(frame.local_instances);
                }
                if (frame.vat_data.handle)
                {
                    vk::buffer::destroy(frame.vat_data);
                }
                if (frame.visible.handle)
                {
                    vk::buffer::destroy(frame.visible);
                }
                if (frame.skin_palette.handle)
                {
                    vk::buffer::destroy(frame.skin_palette);
                }
            }
            mesh.frames.clear();
        }
        if (data.local_pool)
        {
            vk::device::get()->handle.destroyDescriptorPool(data.local_pool);
        }
        data.local_pool = nullptr;
    }

    void create_local_frames()
    {
        auto& data = get();
        const auto& source = weapon::assets();
        if (source.meshes.empty())
        {
            return;
        }
        if (data.local_meshes.empty())
        {
            upload::batch transfers;
            for (const auto& mesh : source.meshes)
            {
                gpu_mesh output;
                output.id = mesh.id;
                output.skin_id = mesh.skin_id;
                output.joint_count = animation::joints(mesh.skin_id);
                output.surface = mesh.surface;
                output.index_count = mesh.indices.size();
                output.vertices = transfers.add(
                    mesh.vertices.data(),
                    mesh.vertices.size() * sizeof(::scene::vertex),
                    vk::BufferUsageFlagBits::eVertexBuffer
                );
                output.indices = transfers.add(
                    mesh.indices.data(),
                    mesh.indices.size() * sizeof(std::uint32_t),
                    vk::BufferUsageFlagBits::eIndexBuffer
                );
                data.local_meshes.push_back(std::move(output));
            }
            transfers.finish();
        }
        const auto count =
            std::uint32_t(data.local_meshes.size() * vk::swapchain::get()->images.size());
        const std::array<vk::DescriptorPoolSize, 2> sizes{
            {{vk::DescriptorType::eStorageBuffer, count * 7},
             {vk::DescriptorType::eCombinedImageSampler, count * 8}}
        };
        vk::DescriptorPoolCreateInfo pool{};
        pool.maxSets = count;
        pool.poolSizeCount = sizes.size();
        pool.pPoolSizes = sizes.data();
        const auto device = vk::device::get()->handle;
        data.local_pool = device.createDescriptorPool(pool);
        for (auto& mesh : data.local_meshes)
        {
            const auto capacity = weapon::instance_capacity(mesh.id);
            std::vector<std::uint32_t> identities(capacity);
            for (std::uint32_t i = 0; i < identities.size(); ++i)
                identities[i] = i;
            mesh.frames.resize(vk::swapchain::get()->images.size());
            for (auto& frame : mesh.frames)
            {
                const auto memory = vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent;
                frame.local_instances = vk::buffer::create(
                    capacity * sizeof(instance_data), vk::BufferUsageFlagBits::eStorageBuffer, memory
                );
                frame.visible = vk::buffer::create(
                    identities.size() * sizeof(identities[0]),
                    vk::BufferUsageFlagBits::eVertexBuffer |
                        vk::BufferUsageFlagBits::eStorageBuffer,
                    memory
                );
                vk::buffer::write(frame.visible, identities.data(), identities.size() * sizeof(identities[0]));
                frame.skin_palette = vk::buffer::create(
                    std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix),
                    vk::BufferUsageFlagBits::eStorageBuffer,
                    memory
                );
                frame.vat_data = vk::buffer::create(
                    capacity * sizeof(vat_gpu::parameters_t),
                    vk::BufferUsageFlagBits::eStorageBuffer, memory
                );
                vk::DescriptorSetAllocateInfo allocate{};
                allocate.descriptorPool = data.local_pool;
                allocate.descriptorSetCount = 1;
                allocate.pSetLayouts = &data.instance_layout;
                frame.set = device.allocateDescriptorSets(allocate).front();
                std::array<vk::DescriptorBufferInfo, 6> infos{};
                std::array<vk::WriteDescriptorSet, 6> writes{};
                for (std::uint32_t i = 0; i < 6; ++i)
                {
                    infos[i].buffer = i == 4 ? frame.skin_palette.handle :
                        i == 5 ? frame.vat_data.handle : frame.local_instances.handle;
                    infos[i].range = VK_WHOLE_SIZE;
                    writes[i].dstSet = frame.set;
                    writes[i].dstBinding = i == 4 ? 9 : i == 5 ? 13 : i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                    writes[i].pBufferInfo = &infos[i];
                }
                device.updateDescriptorSets(writes, {});
                vk::DescriptorBufferInfo wound_info{};
                wound_info.buffer = data.wound_fallback.handle;
                wound_info.range = VK_WHOLE_SIZE;
                vk::WriteDescriptorSet wound_write{};
                wound_write.dstSet = frame.set;
                wound_write.dstBinding = 14;
                wound_write.descriptorCount = 1;
                wound_write.descriptorType = vk::DescriptorType::eStorageBuffer;
                wound_write.pBufferInfo = &wound_info;
                device.updateDescriptorSets({wound_write}, {});
                texture::bind(frame.set, mesh.surface);
                vat_gpu::bind(weapon::vat_type(mesh.id), frame.set);
            }
        }
    }

    void release_frame_resources()
    {
        auto& data = get();
        release_local_frames();
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
            create_local_frames();
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
        mesh.instances.push_back(instance);
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
            vk::DescriptorBufferInfo palette_info{};
            palette_info.buffer = frame.skin_palette.handle;
            palette_info.range = VK_WHOLE_SIZE;
            vk::WriteDescriptorSet palette_write{};
            palette_write.dstSet = frame.set;
            palette_write.dstBinding = 9;
            palette_write.descriptorCount = 1;
            palette_write.descriptorType = vk::DescriptorType::eStorageBuffer;
            palette_write.pBufferInfo = &palette_info;
            vk::device::get()->handle.updateDescriptorSets({palette_write}, {});
            vk::DescriptorBufferInfo vat_info{};
            vat_info.buffer = get().vat_fallback.handle;
            vat_info.range = VK_WHOLE_SIZE;
            vk::WriteDescriptorSet vat_write{};
            vat_write.dstSet = frame.set;
            vat_write.dstBinding = 13;
            vat_write.descriptorCount = 1;
            vat_write.descriptorType = vk::DescriptorType::eStorageBuffer;
            vat_write.pBufferInfo = &vat_info;
            vk::device::get()->handle.updateDescriptorSets({vat_write}, {});
            vk::DescriptorBufferInfo wound_info{};
            wound_info.buffer = frame.wounds.handle
                ? frame.wounds.handle : get().wound_fallback.handle;
            wound_info.range = VK_WHOLE_SIZE;
            vk::WriteDescriptorSet wound_write{};
            wound_write.dstSet = frame.set;
            wound_write.dstBinding = 14;
            wound_write.descriptorCount = 1;
            wound_write.descriptorType = vk::DescriptorType::eStorageBuffer;
            wound_write.pBufferInfo = &wound_info;
            vk::device::get()->handle.updateDescriptorSets({wound_write}, {});
            texture::bind(frame.set, mesh.surface);
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

        const auto palette_size = std::max<std::size_t>(
            sizeof(animation::matrix), mesh.instances.size() *
                std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix));
        for (auto& frame : mesh.frames)
        {
            if (!frame.skin_palette.handle || frame.skin_palette.size < palette_size)
            {
                if (frame.skin_palette.handle) vk::buffer::destroy(frame.skin_palette);
                frame.skin_palette = vk::buffer::create(
                    palette_size, vk::BufferUsageFlagBits::eStorageBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent);
            }
        }
        if (mesh.joint_count)
        {
            const auto wound_bytes = std::max<std::size_t>(
                sizeof(std::uint32_t),
                mesh.instances.size() * wound_atlas_words * sizeof(std::uint32_t));
            for (auto& frame : mesh.frames)
            {
                if (!frame.wounds.handle || frame.wounds.size < wound_bytes)
                {
                    if (frame.wounds.handle) vk::buffer::destroy(frame.wounds);
                    frame.wounds = vk::buffer::create(
                        wound_bytes, vk::BufferUsageFlagBits::eStorageBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible |
                            vk::MemoryPropertyFlagBits::eHostCoherent
                    );
                }
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
        texture::reset();
        texture::synchronize(::scene::state{});
        animation::reset();
        animation::synchronize(weapon::assets());
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
            source->textures.size() >= data.snapshot->textures.size() &&
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
            animation::synchronize(*source);
            if (source->textures.size() != data.snapshot->textures.size())
                texture::synchronize(*source);
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
            animation::synchronize(*source);
            texture::synchronize(*source);
        }
        else
        {
            animation::reset();
            texture::reset();
            texture::synchronize(::scene::state{});
        }

        if (source)
        {
            meshes.reserve(source->meshes.size());

            upload::batch transfers;
            try
            {
                for (const auto& mesh : source->meshes)
                {
                    gpu_mesh result;
                    result.id = mesh.id;
                    result.surface = mesh.surface;
                    result.skin_id = mesh.skin_id;
                    result.joint_count = animation::joints(mesh.skin_id);
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
            catch (...)
            {
                destroy_meshes(meshes);
                throw;
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
                const auto count = std::max<std::size_t>(1, meshes.size()) *
                    std::max<std::size_t>(1, vk::swapchain::get()->images.size());
                std::array<vk::DescriptorPoolSize, 2> pool_sizes{{
                    {vk::DescriptorType::eStorageBuffer, std::uint32_t(count * 7)},
                    {vk::DescriptorType::eCombinedImageSampler, std::uint32_t(count * 8)}
                }};

                vk::DescriptorPoolCreateInfo pool_info{};
                pool_info.maxSets = static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, meshes.size()) *
                    std::max<std::size_t>(1, vk::swapchain::get()->images.size())
                );
                pool_info.poolSizeCount = pool_sizes.size();
                pool_info.pPoolSizes = pool_sizes.data();
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
        for (auto& mesh : data.local_meshes)
        {
            for (auto& frame : mesh.frames)
            {
                texture::bind(frame.set, mesh.surface);
                vat_gpu::bind(weapon::vat_type(mesh.id), frame.set);
            }
        }
    }

    void draw(vk::CommandBuffer command, std::uint32_t image)
    {
        auto& data = get();
        const auto camera = camera::capture();
        const auto& weapon_draws = weapon::update(camera.view);
        const auto extent = vk::swapchain::get()->extent;
        data.last_image = image;
        const float aspect = float(extent.width) / float(extent.height);
        const auto view_projection =
            perspective(camera.pose.fov, aspect, 0.1f, 1000.0f) * camera.view;
        data.last_world_view_projection = view_projection;
        const auto weapon_view_projection = weapon::view_projection(aspect, camera.view);

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

        // Write per-swapchain-image palettes only after its frame fence completes.
        if (data.snapshot)
        {
            const auto server_tick = connection::get()->server_tick.load(
                std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (server_tick != data.last_animation_tick)
            {
                data.last_animation_tick = server_tick;
                data.animation_received = now;
            }
            const auto elapsed = std::chrono::duration<float>(
                now - data.animation_received).count();
            const float smooth_tick = float(server_tick) + std::clamp(elapsed, 0.f, 0.2f) * float(net::limits::tick_hz);
            for (auto& mesh : data.meshes)
            {
                if (!mesh.joint_count || mesh.transforms.empty()) continue;
                std::vector<animation::matrix> matrices;
                matrices.reserve(mesh.instances.size() * mesh.joint_count);
                for (const auto& instance : mesh.instances)
                {
                    std::vector<animation::matrix> bones;
                    const auto update = connection::animation(instance.id);
                    const auto start = update ? update->start_tick : instance.animation_tick;
                    const auto clip = update ? update->clip : instance.animation;
                    const auto overlay = update ? update->overlay : instance.overlay_animation;
                    const auto weight = update ? update->blend : instance.overlay_weight;
                    const auto root = update ? update->mask_root : instance.overlay_root;
                    const float time = (smooth_tick - float(start)) / float(net::limits::tick_hz);
                    if (!animation::sample(mesh.skin_id, clip, std::max(0.f, time), bones,
                        overlay, weight, root) ||
                        bones.size() != mesh.joint_count)
                        throw std::runtime_error("Cannot sample streamed animation");
                    matrices.insert(matrices.end(), bones.begin(), bones.end());
                }
                vk::buffer::write(mesh.frames.at(image).skin_palette,
                                  matrices.data(), matrices.size() * sizeof(matrices[0]));
            }
        }

        // One packed R8 UV atlas per skinned actor (128x128, 16 KiB each).
        // Each texel stores 4-bit coverage and 4-bit wetness. Atlas index is
        // exactly the skinning instance index consumed by the vertex shader.
        const auto active_wounds = weapon::impact_wounds();
        if (active_wounds.empty())
        {
            for (auto& mesh : data.meshes) mesh.has_wounds = false;
        }
        else
        {
        for (auto& mesh : data.meshes)
        {
            if (!mesh.joint_count || mesh.instances.empty()) continue;
            auto& frame = mesh.frames.at(image);
            if (!frame.wounds.handle) continue;

            std::unordered_map<std::uint32_t, std::size_t> owners;
            owners.reserve(mesh.instances.size());
            for (std::size_t i = 0; i < mesh.instances.size(); ++i)
                owners.emplace(mesh.instances[i].id, i);

            mesh.has_wounds = std::any_of(
                active_wounds.begin(), active_wounds.end(),
                [&](const impact::wound& item) { return owners.contains(item.target); });
            if (!mesh.has_wounds) continue;

            std::vector<std::uint32_t> atlas(
                mesh.instances.size() * wound_atlas_words, 0u);
            for (const auto& wound : active_wounds)
            {
                const auto owner = owners.find(wound.target);
                if (owner == owners.end()) continue;

                const float radius = std::max(
                    1.25f, wound.radius * float(wound_atlas_resolution));
                const float cx = wound.uv.x * float(wound_atlas_resolution - 1);
                const float cy = wound.uv.y * float(wound_atlas_resolution - 1);
                const int padding = int(std::ceil(radius * 1.5f));
                const int x0 = std::max(0, int(cx) - padding);
                const int y0 = std::max(0, int(cy) - padding);
                const int x1 = std::min(127, int(cx) + padding);
                const int y1 = std::min(127, int(cy) + padding);
                const float wetness = std::clamp(1.f - wound.age / 100.f, 0.f, 1.f);

                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        const float dx = (float(x) + .5f - cx) / radius;
                        const float dy = (float(y) + .5f - cy) / radius;
                        const float angle = std::atan2(dy, dx);
                        const float direction =
                            std::cos(angle - wound.angle) * .3f;
                        const float distance = std::sqrt(
                            dx * dx * (1.1f + direction) +
                            dy * dy * (1.1f - direction));
                        const float ragged = .84f +
                            .1f * std::sin(angle * 7.f + wound.angle) +
                            .06f * std::cos(angle * 13.f);
                        const float shape = std::clamp(
                            (ragged - distance) * 4.0f, 0.f, 1.f) *
                            wound.strength;
                        const auto coverage = std::uint32_t(std::clamp(
                            int(std::lround(shape * 15.f)), 0, 15));
                        if (!coverage) continue;

                        const std::size_t pixel = std::size_t(y) * wound_atlas_resolution +
                                                  std::size_t(x);
                        const std::size_t index =
                            owner->second * wound_atlas_words + pixel / 4;
                        const auto bits = std::uint32_t((pixel & 3u) * 8u);
                        const auto old = (atlas[index] >> bits) & 255u;
                        const auto previous_coverage = old >> 4u;
                        if (coverage < previous_coverage) continue;
                        const auto moisture = std::uint32_t(std::clamp(
                            int(std::lround(wetness * 15.f)), 0, 15));
                        const auto stamp = (coverage << 4u) |
                            std::max(moisture, coverage == previous_coverage ?
                                (old & 15u) : 0u);
                        atlas[index] = (atlas[index] & ~(255u << bits)) |
                            (stamp << bits);
                    }
            }
            vk::buffer::write(frame.wounds, atlas.data(),
                              atlas.size() * sizeof(atlas.front()));
        }

        }

        for (const auto& item : weapon_draws)
        {
            auto found = std::find_if(
                data.local_meshes.begin(),
                data.local_meshes.end(),
                [&](const gpu_mesh& mesh)
                {
                    return mesh.id == item.mesh_id;
                }
            );
            if (found == data.local_meshes.end())
            {
                continue;
            }
            auto& frame = found->frames.at(image);
            if (item.instances.size() > weapon::instance_capacity(item.mesh_id))
                throw std::runtime_error("Local effect instance capacity exceeded");
            if (!item.instances.empty())
            {
                vk::buffer::write(
                    frame.local_instances,
                    item.instances.data(),
                    item.instances.size() * sizeof(weapon::instance_data)
                );
            }
            if (!item.vat.empty())
            {
                vk::buffer::write(
                    frame.vat_data, item.vat.data(),
                    item.vat.size() * sizeof(vat_gpu::parameters_t)
                );
            }
            if (!item.palette.empty())
            {
                vk::buffer::write(
                    frame.skin_palette,
                    item.palette.data(),
                    item.palette.size() * sizeof(animation::matrix)
                );
            }
        }
        command.beginRenderPass(begin, vk::SubpassContents::eInline);

        command.bindPipeline(
            vk::PipelineBindPoint::eGraphics,
            data.pipeline
        );

        command.setViewport(
            0, vk::Viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.1f, 1.0f}
        );

        command.setScissor(0, vk::Rect2D{{0, 0}, extent});

        if (data.snapshot)
        {
            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty())
                {
                    continue;
                }

                const auto& m = mesh.surface;
                const auto flags =
                    (m.base_color_texture ? 1u : 0u) |
                    (m.metallic_roughness_texture ? 2u : 0u) |
                    (m.normal_texture ? 4u : 0u) |
                    (m.occlusion_texture ? 8u : 0u) |
                    (m.emissive_texture ? 16u : 0u) |
                    (m.alpha_mode << 5);
                draw_parameters params{
                    view_projection,
                    m.base_color,
                    {m.metallic, m.roughness, m.occlusion, m.alpha_cutoff},
                    {m.emissive[0], m.emissive[1],
                     m.emissive[2], float(flags)},
                    mesh.joint_count,
                    mesh.has_wounds ? 1u : 0u
                };
                command.pushConstants(
                    data.layout, vk::ShaderStageFlagBits::eVertex |
                    vk::ShaderStageFlagBits::eFragment,
                    0, sizeof(params), &params
                );
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

        // Disjoint depth ranges give the viewmodel independent depth testing while
        // preserving world depth for deferred HDR lighting and sky classification.
        for (bool viewmodel : {false, true})
        {
            command.setViewport(
                0,
                vk::Viewport{
                    0,
                    0,
                    float(extent.width),
                    float(extent.height),
                    viewmodel ? 0.f : .1f,
                    viewmodel ? .099f : 1.f
                }
            );
            for (const auto& item : weapon_draws)
            {
                if (item.viewmodel != viewmodel)
                {
                    continue;
                }
                const auto found = std::find_if(
                    data.local_meshes.begin(),
                    data.local_meshes.end(),
                    [&](const gpu_mesh& mesh)
                    {
                        return mesh.id == item.mesh_id;
                    }
                );
                if (found == data.local_meshes.end())
                {
                    continue;
                }
                const auto& mesh = *found;
                const auto& frame = mesh.frames.at(image);
                const auto& m = mesh.surface;
                const auto flags = (m.base_color_texture ? 1u : 0u) |
                                   (m.metallic_roughness_texture ? 2u : 0u) |
                                   (m.normal_texture ? 4u : 0u) | (m.occlusion_texture ? 8u : 0u) |
                                   (m.emissive_texture ? 16u : 0u) | (m.alpha_mode << 5);
                draw_parameters params{
                    viewmodel ? weapon_view_projection : view_projection,
                    m.base_color,
                    {m.metallic, m.roughness, m.occlusion, m.alpha_cutoff},
                    {m.emissive[0], m.emissive[1], m.emissive[2], float(flags)},
                    item.vat.empty() ? mesh.joint_count : UINT32_MAX,
                    0u
                };
                command.pushConstants(
                    data.layout,
                    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                    0,
                    sizeof(params),
                    &params
                );
                command.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics, data.layout, 0, {frame.set}, {}
                );
                command.bindVertexBuffers(
                    0,
                    std::array<vk::Buffer, 2>{mesh.vertices.handle, frame.visible.handle},
                    std::array<vk::DeviceSize, 2>{0, 0}
                );
                command.bindIndexBuffer(mesh.indices.handle, 0, vk::IndexType::eUint32);
                command.drawIndexed(mesh.index_count, item.instances.size(), 0, 0, 0);
            }
        }

        command.endRenderPass();
    }

    void shutdown()
    {
        auto& data = get();

        destroy_meshes(data.meshes);
        release_local_frames();
        destroy_meshes(data.local_meshes);
        weapon::shutdown();
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
        vat_gpu::shutdown();
        if (data.vat_fallback.handle)
        {
            vk::buffer::destroy(data.vat_fallback);
            data.vat_fallback = {};
        }
        if (data.wound_fallback.handle)
        {
            vk::buffer::destroy(data.wound_fallback);
            data.wound_fallback = {};
        }
        texture::shutdown();
        if (data.instance_layout)
        {
            vk::device::get()->handle.destroyDescriptorSetLayout(data.instance_layout);
            data.instance_layout = nullptr;
        }
    }
} // namespace clt::scene_renderer
