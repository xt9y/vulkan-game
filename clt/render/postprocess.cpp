#include "postprocess.hpp"
#include "scene_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace clt::postprocess
{
    namespace
    {
        struct image_target {
            vk::image::resource image;
            vk::ImageView view{};
        };

        struct frame {
            image_target hdr;
            image_target ldr;
            vk::ImageView swap_view{};
            vk::Framebuffer framebuffer{};
            vk::DescriptorSet lighting{};
            vk::DescriptorSet tone{};
            vk::DescriptorSet presentation{};
            bool initialized = false;
        };

        struct state {
            std::vector<frame> frames;
            vk::Sampler sampler{};
            vk::DescriptorSetLayout lighting_set{};
            vk::DescriptorSetLayout tone_set{};
            vk::DescriptorSetLayout present_set{};
            vk::DescriptorPool pool{};
            vk::PipelineLayout lighting_layout{};
            vk::PipelineLayout tone_layout{};
            vk::PipelineLayout present_layout{};
            vk::Pipeline lighting_pipeline{};
            vk::Pipeline tone_pipeline{};
            vk::Pipeline present_pipeline{};
            vk::RenderPass render_pass{};
        };

        state& get()
        {
            static state data;
            return data;
        }

        std::atomic<bool> use_lighting{true};
        std::atomic<bool> use_tonemapping{true};
        std::atomic<float> exposure_value{1.0f};
        std::once_flag environment;

        struct push {
            std::int32_t enabled = 1;
            float exposure = 1.0f;
        };

        vk::ShaderModule shader(const char* filename)
        {
            return vk::shader::create(vk::shader::read(filename));
        }

        vk::DescriptorSetLayout layout(
            std::uint32_t sampled, std::uint32_t storage, vk::ShaderStageFlags stages
        )
        {
            std::vector<vk::DescriptorSetLayoutBinding> bindings(sampled + storage);
            for (std::uint32_t i = 0; i < bindings.size(); ++i)
            {
                auto& b = bindings[i];
                b.binding = i;
                b.descriptorCount = 1;
                b.stageFlags = stages;
                b.descriptorType = i < sampled
                    ? vk::DescriptorType::eCombinedImageSampler
                    : vk::DescriptorType::eStorageImage;
            }
            vk::DescriptorSetLayoutCreateInfo info{};
            info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            return vk::device::get()->handle.createDescriptorSetLayout(info);
        }

        vk::PipelineLayout pipeline_layout(vk::DescriptorSetLayout set, bool compute)
        {
            vk::PipelineLayoutCreateInfo info{};
            info.setLayoutCount = 1;
            info.pSetLayouts = &set;
            vk::PushConstantRange constants{};
            if (compute)
            {
                constants.offset = 0;
                constants.size = sizeof(push);
                constants.stageFlags = vk::ShaderStageFlagBits::eCompute;
                info.pushConstantRangeCount = 1;
                info.pPushConstantRanges = &constants;
            }
            return vk::device::get()->handle.createPipelineLayout(info);
        }

        vk::Pipeline compute_pipeline(const char* name, vk::PipelineLayout layout)
        {
            auto module = shader(name);
            try
            {
                vk::ComputePipelineCreateInfo info{};
                info.stage.stage = vk::ShaderStageFlagBits::eCompute;
                info.stage.module = module;
                info.stage.pName = "main";
                info.layout = layout;
                auto result = vk::pipeline::compute(info);
                vk::shader::destroy(module);
                return result;
            }
            catch (...)
            {
                vk::shader::destroy(module);
                throw;
            }
        }

        image_target make_target(vk::Format format)
        {
            const auto extent = vk::swapchain::get()->extent;
            image_target result;
            vk::ImageCreateInfo info{};
            info.imageType = vk::ImageType::e2D;
            info.format = format;
            info.extent = {extent.width, extent.height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = vk::SampleCountFlagBits::e1;
            info.tiling = vk::ImageTiling::eOptimal;
            info.usage = vk::ImageUsageFlagBits::eStorage |
                vk::ImageUsageFlagBits::eSampled;
            result.image = vk::image::create(info, vk::MemoryPropertyFlagBits::eDeviceLocal);

            try
            {
                vk::ImageViewCreateInfo view{};
                view.image = result.image.handle;
                view.viewType = vk::ImageViewType::e2D;
                view.format = format;
                view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                result.view = vk::image_view::create(view);
            }
            catch (...)
            {
                vk::image::destroy(result.image);
                throw;
            }
            return result;
        }

        void destroy_target(image_target& target)
        {
            if (target.view) vk::image_view::destroy(target.view);
            if (target.image.handle) vk::image::destroy(target.image);
            target = {};
        }

        void update(vk::DescriptorSet set, std::vector<vk::DescriptorImageInfo> images,
                    std::size_t sampled)
        {
            std::vector<vk::WriteDescriptorSet> writes(images.size());
            for (std::uint32_t i = 0; i < writes.size(); ++i)
            {
                writes[i].dstSet = set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = i < sampled
                    ? vk::DescriptorType::eCombinedImageSampler
                    : vk::DescriptorType::eStorageImage;
                writes[i].pImageInfo = &images[i];
            }
            vk::device::get()->handle.updateDescriptorSets(writes, {});
        }

        vk::Pipeline presentation_pipeline()
        {
            const auto& data = get();
            const auto vertex = shader("shd/fullscreen.vert.spv");
            const auto fragment = shader("shd/present.frag.spv");
            try
            {
                std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
                stages[0].stage = vk::ShaderStageFlagBits::eVertex;
                stages[0].module = vertex;
                stages[0].pName = "main";
                stages[1].stage = vk::ShaderStageFlagBits::eFragment;
                stages[1].module = fragment;
                stages[1].pName = "main";

                vk::PipelineVertexInputStateCreateInfo input{};
                vk::PipelineInputAssemblyStateCreateInfo assembly{};
                assembly.topology = vk::PrimitiveTopology::eTriangleList;
                vk::PipelineViewportStateCreateInfo viewport{};
                viewport.viewportCount = 1;
                viewport.scissorCount = 1;
                vk::PipelineRasterizationStateCreateInfo raster{};
                raster.polygonMode = vk::PolygonMode::eFill;
                raster.cullMode = vk::CullModeFlagBits::eNone;
                raster.frontFace = vk::FrontFace::eCounterClockwise;
                raster.lineWidth = 1;
                vk::PipelineMultisampleStateCreateInfo multi{};
                multi.rasterizationSamples = vk::SampleCountFlagBits::e1;
                vk::PipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask = vk::ColorComponentFlagBits::eR |
                    vk::ColorComponentFlagBits::eG |
                    vk::ColorComponentFlagBits::eB |
                    vk::ColorComponentFlagBits::eA;
                vk::PipelineColorBlendStateCreateInfo blending{};
                blending.attachmentCount = 1;
                blending.pAttachments = &blend;
                std::array<vk::DynamicState, 2> dynamics{
                    vk::DynamicState::eViewport, vk::DynamicState::eScissor
                };
                vk::PipelineDynamicStateCreateInfo dynamic{};
                dynamic.dynamicStateCount = dynamics.size();
                dynamic.pDynamicStates = dynamics.data();

                vk::GraphicsPipelineCreateInfo info{};
                info.stageCount = stages.size();
                info.pStages = stages.data();
                info.pVertexInputState = &input;
                info.pInputAssemblyState = &assembly;
                info.pViewportState = &viewport;
                info.pRasterizationState = &raster;
                info.pMultisampleState = &multi;
                info.pColorBlendState = &blending;
                info.pDynamicState = &dynamic;
                info.layout = data.present_layout;
                info.renderPass = data.render_pass;
                const auto pipeline = vk::pipeline::graphics(info);
                vk::shader::destroy(vertex);
                vk::shader::destroy(fragment);
                return pipeline;
            }
            catch (...)
            {
                vk::shader::destroy(vertex);
                vk::shader::destroy(fragment);
                throw;
            }
        }

        void transition(vk::CommandBuffer command, image_target& target)
        {
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eGeneral;
            barrier.image = target.image.handle;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {}, {barrier}
            );
        }

        void barrier(vk::CommandBuffer command,
                     vk::PipelineStageFlags destination, vk::AccessFlags access)
        {
            vk::MemoryBarrier memory{};
            memory.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            memory.dstAccessMask = access;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader, destination,
                {}, {memory}, {}, {}
            );
        }

        void compute(vk::CommandBuffer command, vk::Pipeline pipeline,
                     vk::PipelineLayout layout, vk::DescriptorSet set,
                     bool enabled)
        {
            const auto extent = vk::swapchain::get()->extent;
            const push controls{
                enabled ? 1 : 0, exposure_value.load(std::memory_order_relaxed)
            };
            command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, {set}, {});
            command.pushConstants(
                layout, vk::ShaderStageFlagBits::eCompute, 0,
                sizeof(controls), &controls
            );
            command.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        }
    }

    bool enabled(effect value)
    {
        return (value == effect::lighting ? use_lighting : use_tonemapping)
            .load(std::memory_order_relaxed);
    }

    void set(effect value, bool active)
    {
        (value == effect::lighting ? use_lighting : use_tonemapping)
            .store(active, std::memory_order_relaxed);
    }

    void toggle(effect value)
    {
        set(value, !enabled(value));
    }

    void set_exposure(float exposure)
    {
        exposure_value.store(std::max(0.0f, exposure), std::memory_order_relaxed);
    }

    void create()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        const auto gpu = vk::physical_device::get()->handle;
        const auto required = vk::FormatFeatureFlagBits::eStorageImage |
            vk::FormatFeatureFlagBits::eSampledImage;
        for (auto format : {vk::Format::eR16G16B16A16Sfloat, vk::Format::eR8G8B8A8Unorm})
        {
            const auto support = gpu.getFormatProperties(format).optimalTilingFeatures;
            if ((support & required) != required)
                throw std::runtime_error("Required offscreen storage image format unsupported");
        }

        std::call_once(environment, []
        {
            if (const char* setting = std::getenv("VK_LIGHTING"))
                use_lighting.store(setting[0] != '0', std::memory_order_relaxed);
            if (const char* setting = std::getenv("VK_TONEMAP"))
                use_tonemapping.store(setting[0] != '0', std::memory_order_relaxed);
        });

        try
        {
            vk::SamplerCreateInfo sampler{};
            sampler.magFilter = vk::Filter::eNearest;
            sampler.minFilter = vk::Filter::eNearest;
            sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
            sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            data.sampler = device.createSampler(sampler);

            data.lighting_set = layout(4, 1, vk::ShaderStageFlagBits::eCompute);
            data.tone_set = layout(1, 1, vk::ShaderStageFlagBits::eCompute);
            data.present_set = layout(1, 0, vk::ShaderStageFlagBits::eFragment);
            data.lighting_layout = pipeline_layout(data.lighting_set, true);
            data.tone_layout = pipeline_layout(data.tone_set, true);
            data.present_layout = pipeline_layout(data.present_set, false);

            data.lighting_pipeline = compute_pipeline("shd/light.comp.spv", data.lighting_layout);
            data.tone_pipeline = compute_pipeline("shd/tone.comp.spv", data.tone_layout);

            vk::AttachmentDescription attachment{};
            attachment.format = vk::swapchain::get()->format;
            attachment.samples = vk::SampleCountFlagBits::e1;
            attachment.loadOp = vk::AttachmentLoadOp::eClear;
            attachment.storeOp = vk::AttachmentStoreOp::eStore;
            attachment.initialLayout = vk::ImageLayout::eUndefined;
            attachment.finalLayout = vk::ImageLayout::ePresentSrcKHR;
            vk::AttachmentReference color{0, vk::ImageLayout::eColorAttachmentOptimal};
            vk::SubpassDescription subpass{};
            subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color;
            vk::RenderPassCreateInfo info{};
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            data.render_pass = vk::render_pass::create(info);
            data.present_pipeline = presentation_pipeline();

            const auto count = static_cast<std::uint32_t>(
                vk::swapchain::get()->images.size()
            );
            std::array<vk::DescriptorPoolSize, 2> pool_sizes{{
                {vk::DescriptorType::eCombinedImageSampler, 6 * count},
                {vk::DescriptorType::eStorageImage, 2 * count}
            }};
            vk::DescriptorPoolCreateInfo pool{};
            pool.maxSets = 3 * count;
            pool.poolSizeCount = pool_sizes.size();
            pool.pPoolSizes = pool_sizes.data();
            data.pool = device.createDescriptorPool(pool);
            data.frames.reserve(count);

            for (std::uint32_t i = 0; i < count; ++i)
            {
                frame item;
                data.frames.push_back(item);
                auto& current = data.frames.back();
                current.hdr = make_target(vk::Format::eR16G16B16A16Sfloat);
                current.ldr = make_target(vk::Format::eR8G8B8A8Unorm);

                vk::ImageViewCreateInfo swap_view{};
                swap_view.image = vk::swapchain::get()->images[i];
                swap_view.viewType = vk::ImageViewType::e2D;
                swap_view.format = vk::swapchain::get()->format;
                swap_view.subresourceRange = {
                    vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
                };
                current.swap_view = vk::image_view::create(swap_view);

                vk::FramebufferCreateInfo framebuffer{};
                framebuffer.renderPass = data.render_pass;
                framebuffer.attachmentCount = 1;
                framebuffer.pAttachments = &current.swap_view;
                framebuffer.width = vk::swapchain::get()->extent.width;
                framebuffer.height = vk::swapchain::get()->extent.height;
                framebuffer.layers = 1;
                current.framebuffer = vk::framebuffer::create(framebuffer);

                const std::array<vk::DescriptorSetLayout, 3> sets{
                    data.lighting_set, data.tone_set, data.present_set
                };
                vk::DescriptorSetAllocateInfo allocation{};
                allocation.descriptorPool = data.pool;
                allocation.descriptorSetCount = sets.size();
                allocation.pSetLayouts = sets.data();
                auto allocated = device.allocateDescriptorSets(allocation);
                current.lighting = allocated[0];
                current.tone = allocated[1];
                current.presentation = allocated[2];

                const auto g = scene_renderer::outputs(i);
                std::vector<vk::DescriptorImageInfo> lighting(5);
                for (std::size_t j = 0; j < g.views.size(); ++j)
                {
                    lighting[j].sampler = data.sampler;
                    lighting[j].imageView = g.views[j];
                    lighting[j].imageLayout = j == 3 ?
                        vk::ImageLayout::eDepthStencilReadOnlyOptimal :
                        vk::ImageLayout::eShaderReadOnlyOptimal;
                }
                lighting[4].imageView = current.hdr.view;
                lighting[4].imageLayout = vk::ImageLayout::eGeneral;
                update(current.lighting, lighting, 4);
                std::vector<vk::DescriptorImageInfo> tone(2);
                tone[0] = {data.sampler, current.hdr.view, vk::ImageLayout::eGeneral};
                tone[1] = {{}, current.ldr.view, vk::ImageLayout::eGeneral};
                update(current.tone, tone, 1);
                std::vector<vk::DescriptorImageInfo> presented{{
                    data.sampler, current.ldr.view, vk::ImageLayout::eGeneral
                }};
                update(current.presentation, presented, 1);
            }
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    void release()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        for (auto& frame : data.frames)
        {
            if (frame.framebuffer) vk::framebuffer::destroy(frame.framebuffer);
            if (frame.swap_view) vk::image_view::destroy(frame.swap_view);
            destroy_target(frame.hdr);
            destroy_target(frame.ldr);
        }
        data.frames.clear();

        vk::pipeline::destroy(data.lighting_pipeline);
        vk::pipeline::destroy(data.tone_pipeline);
        vk::pipeline::destroy(data.present_pipeline);
        if (data.render_pass) vk::render_pass::destroy(data.render_pass);
        if (data.lighting_layout) device.destroyPipelineLayout(data.lighting_layout);
        if (data.tone_layout) device.destroyPipelineLayout(data.tone_layout);
        if (data.present_layout) device.destroyPipelineLayout(data.present_layout);
        if (data.pool) device.destroyDescriptorPool(data.pool);
        if (data.lighting_set) device.destroyDescriptorSetLayout(data.lighting_set);
        if (data.tone_set) device.destroyDescriptorSetLayout(data.tone_set);
        if (data.present_set) device.destroyDescriptorSetLayout(data.present_set);
        if (data.sampler) device.destroySampler(data.sampler);

        data = {};
    }

    void lighting(vk::CommandBuffer command, std::uint32_t image)
    {
        auto& current = get().frames.at(image);
        if (!current.initialized)
        {
            transition(command, current.hdr);
            transition(command, current.ldr);
            current.initialized = true;
        }
        compute(command, get().lighting_pipeline, get().lighting_layout,
                current.lighting, enabled(effect::lighting));
    }

    void tonemap(vk::CommandBuffer command, std::uint32_t image)
    {
        barrier(command, vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead);
        const auto& current = get().frames.at(image);
        compute(command, get().tone_pipeline, get().tone_layout,
                current.tone, enabled(effect::tonemapping));
    }

    void present(vk::CommandBuffer command, std::uint32_t image)
    {
        barrier(command, vk::PipelineStageFlagBits::eFragmentShader,
                vk::AccessFlagBits::eShaderRead);

        const auto extent = vk::swapchain::get()->extent;
        vk::ClearValue clear{};
        clear.color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 1});
        vk::RenderPassBeginInfo info{};
        info.renderPass = get().render_pass;
        info.framebuffer = get().frames.at(image).framebuffer;
        info.renderArea = vk::Rect2D{{0, 0}, extent};
        info.clearValueCount = 1;
        info.pClearValues = &clear;

        command.beginRenderPass(info, vk::SubpassContents::eInline);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, get().present_pipeline);
        command.setViewport(0, vk::Viewport{
            0, 0, float(extent.width), float(extent.height), 0, 1
        });
        command.setScissor(0, vk::Rect2D{{0, 0}, extent});
        command.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, get().present_layout,
            0, {get().frames.at(image).presentation}, {}
        );
        command.draw(3, 1, 0, 0);
        command.endRenderPass();
    }
}
