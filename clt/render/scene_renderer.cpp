#include "scene_renderer.hpp"

#include "clt/network/connection.hpp"
#include "clt/view/camera.hpp"
#include "srv/shared/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace clt::scene_renderer
{
    namespace
    {
        using namespace vk::util::math;

        struct gpu_mesh {
            std::uint32_t id = 0;
            vk::buffer::resource vertices;
            vk::buffer::resource indices;
            std::uint32_t index_count = 0;
        };

        struct push_constants {
            mat4 mvp;
            mat4 normal;
        };

        static_assert(sizeof(push_constants) == 128);
        static_assert(sizeof(::scene::vertex) == 32);

        struct state {
            std::shared_ptr<const ::scene::state> snapshot;
            std::vector<gpu_mesh> meshes;

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
            }

            meshes.clear();
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
                range.size = sizeof(push_constants);

                vk::PipelineLayoutCreateInfo layout_info{};
                layout_info.pushConstantRangeCount = 1;
                layout_info.pPushConstantRanges = &range;

                data.layout = vk::pipeline_layout::create(layout_info);

                std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};

                stages[0].stage = vk::ShaderStageFlagBits::eVertex;
                stages[0].module = vertex_shader;
                stages[0].pName = "main";

                stages[1].stage = vk::ShaderStageFlagBits::eFragment;
                stages[1].module = fragment_shader;
                stages[1].pName = "main";

                vk::VertexInputBindingDescription binding{};
                binding.binding = 0;
                binding.stride = sizeof(::scene::vertex);
                binding.inputRate = vk::VertexInputRate::eVertex;

                std::array<vk::VertexInputAttributeDescription, 2> attributes{};

                attributes[0].location = 0;
                attributes[0].binding = 0;
                attributes[0].format = vk::Format::eR32G32B32Sfloat;
                attributes[0].offset = offsetof(::scene::vertex, position);

                attributes[1].location = 1;
                attributes[1].binding = 0;
                attributes[1].format = vk::Format::eR32G32B32Sfloat;
                attributes[1].offset = offsetof(::scene::vertex, normal);

                vk::PipelineVertexInputStateCreateInfo vertex_input{};
                vertex_input.vertexBindingDescriptionCount = 1;
                vertex_input.pVertexBindingDescriptions = &binding;
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

    void synchronize()
    {
        auto& data = get();
        const auto source = connection::snapshot();

        if (source == data.snapshot)
        {
            return;
        }

        std::vector<gpu_mesh> meshes;

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

                    result.vertices = vk::buffer::create(
                        vertex_bytes,
                        vk::BufferUsageFlagBits::eVertexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent
                    );

                    result.indices = vk::buffer::create(
                        index_bytes,
                        vk::BufferUsageFlagBits::eIndexBuffer,
                        vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent
                    );

                    vk::buffer::write(
                        result.vertices,
                        mesh.vertices.data(),
                        vertex_bytes
                    );

                    vk::buffer::write(
                        result.indices,
                        mesh.indices.data(),
                        index_bytes
                    );

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

        destroy_meshes(data.meshes);
        data.meshes = std::move(meshes);
        data.snapshot = source;
    }

    void draw(vk::CommandBuffer command, std::uint32_t image)
    {
        auto& data = get();
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

            for (const auto& instance : data.snapshot->instances)
            {
                const auto it = std::find_if(
                    data.meshes.begin(),
                    data.meshes.end(),
                    [&](const gpu_mesh& mesh)
                    {
                        return mesh.id == instance.mesh_id;
                    }
                );

                if (it == data.meshes.end())
                {
                    continue;
                }

                const auto& pose = instance.pose;

                const auto model = trs(
                    pose.position,
                    pose.rotation,
                    pose.scale
                );

                const vec3 inverse_scale{
                    pose.scale.x != 0 ? 1.0f / pose.scale.x : 0.0f,
                    pose.scale.y != 0 ? 1.0f / pose.scale.y : 0.0f,
                    pose.scale.z != 0 ? 1.0f / pose.scale.z : 0.0f
                };

                const push_constants constants{
                    view_projection * model,
                    rotation(pose.rotation) * scale(inverse_scale)
                };

                command.bindVertexBuffers(
                    0,
                    it->vertices.handle,
                    vk::DeviceSize(0)
                );

                command.bindIndexBuffer(
                    it->indices.handle,
                    0,
                    vk::IndexType::eUint32
                );

                command.pushConstants(
                    data.layout,
                    vk::ShaderStageFlagBits::eVertex,
                    0,
                    sizeof(constants),
                    &constants
                );

                command.drawIndexed(it->index_count, 1, 0, 0, 0);
            }
        }

        command.endRenderPass();
    }

    void shutdown()
    {
        auto& data = get();

        destroy_meshes(data.meshes);
        data.snapshot.reset();

        release_frame_resources();
    }
} // namespace clt::scene_renderer
