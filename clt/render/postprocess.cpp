#include "postprocess.hpp"
#include "srv/game/options.hpp"
#include "scene_renderer.hpp"
#include "clt/view/camera.hpp"
#include <render/renderer.hpp>
#include <render/pipeline.hpp>
#include <gpu/compute.hpp>
#include <gpu/descriptors.hpp>
#include <render/postprocess.hpp>
#include <memory>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <array>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace clt::postprocess
{
    namespace
    {
        struct image_target {
            vk::Image image{};
            vk::ImageView view{};
        };

        struct frame {
            image_target hdr;
            image_target ldr;
            vk::DescriptorSet lighting{};
            vk::DescriptorSet tone{};
            vk::DescriptorSet presentation{};
        };

        struct state {
            std::vector<frame> frames;
            vk::Sampler sampler{};
            std::unique_ptr<gpu::descriptors> lighting_sets;
            std::unique_ptr<gpu::descriptors> tone_sets;
            std::unique_ptr<gpu::descriptors> present_sets;
            std::unique_ptr<::render::postprocess_targets> targets;
            std::unique_ptr<::render::presentation_targets> presentation;
            vk::PipelineLayout present_layout{};
            std::unique_ptr<gpu::compute> lighting_program;
            std::unique_ptr<gpu::compute> tone_program;
            std::unique_ptr<::render::pipeline> present_program;
        };

        state& get()
        {
            static state data;
            return data;
        }

        std::atomic<bool> use_lighting{true};
        std::atomic<bool> use_tonemapping{true};
        std::atomic<float> exposure_value{};

        struct push {
            std::int32_t enabled;
            float exposure;
            std::array<float, 2> padding{};
            std::array<float, 4> view_direction{};
            std::array<float, 4> sun{};
            std::array<float, 4> ambient{};
            std::array<float, 4> sky{};
        };

        std::vector<vk::DescriptorSetLayoutBinding> bindings(
            std::uint32_t sampled,std::uint32_t storage,vk::ShaderStageFlags stages)
        {
            std::vector<vk::DescriptorSetLayoutBinding> result(sampled+storage);
            for(std::uint32_t i=0;i<result.size();++i) {
                auto& b=result[i];
                b.binding=i;
                b.descriptorCount=1;
                b.stageFlags=stages;
                b.descriptorType=i<sampled?
                    vk::DescriptorType::eCombinedImageSampler:
                    vk::DescriptorType::eStorageImage;
            }
            return result;
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

        void compute(vk::CommandBuffer command, const gpu::compute& program,
                     vk::DescriptorSet set, bool enabled)
        {
            const auto extent = vk::swapchain::get()->extent;
            const auto view = camera::snapshot();
            const auto& lighting = game::options::get()->graphics;
            const push controls{
                enabled ? 1 : 0, exposure_value.load(std::memory_order_relaxed),
                {},
                {-std::sin(view.yaw) * std::cos(view.pitch),
                 -std::sin(view.pitch),
                 std::cos(view.yaw) * std::cos(view.pitch), 0},
                {lighting.sun_direction.x, lighting.sun_direction.y,
                 lighting.sun_direction.z, lighting.sun_intensity},
                {0.08f, 0.02f, 0, 0},
                {0.035f, 0.045f, 0.065f, 1}
            };
            program.dispatch(
                command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1,
                {set}, &controls, sizeof(controls));
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

            const auto count=static_cast<std::uint32_t>(vk::swapchain::get()->images.size());
            data.targets=std::make_unique<::render::postprocess_targets>();
            data.targets->create(count,vk::swapchain::get()->extent);
            data.presentation=std::make_unique<::render::presentation_targets>();
            data.presentation->create(vk::swapchain::get()->images,
                vk::swapchain::get()->format,vk::swapchain::get()->extent);
            data.lighting_sets=std::make_unique<gpu::descriptors>();
            data.lighting_sets->create(bindings(4,1,vk::ShaderStageFlagBits::eCompute),count);
            data.tone_sets=std::make_unique<gpu::descriptors>();
            data.tone_sets->create(bindings(1,1,vk::ShaderStageFlagBits::eCompute),count);
            data.present_sets=std::make_unique<gpu::descriptors>();
            data.present_sets->create(bindings(1,0,vk::ShaderStageFlagBits::eFragment),count);
            data.present_layout=pipeline_layout(data.present_sets->layout(),false);
            data.lighting_program = std::make_unique<gpu::compute>();
            data.lighting_program->create(
                game::options::get()->graphics.lighting_shader,
                {data.lighting_sets->layout()}, sizeof(push));
            data.tone_program = std::make_unique<gpu::compute>();
            data.tone_program->create(
                game::options::get()->graphics.tonemap_shader,
                {data.tone_sets->layout()}, sizeof(push));

            data.present_program = std::make_unique<::render::pipeline>();
            ::render::raster_state raster{};
            raster.pass = data.presentation->pass();
            raster.layout = data.present_layout;
            raster.cull = vk::CullModeFlagBits::eNone;
            raster.depth_test = false;
            raster.depth_write = false;
            data.present_program->create(raster, {
                {vk::ShaderStageFlagBits::eVertex,
                 game::options::get()->graphics.fullscreen_shader},
                {vk::ShaderStageFlagBits::eFragment,
                 game::options::get()->graphics.presentation_shader}
            });

            data.frames.reserve(count);

            for (std::uint32_t i = 0; i < count; ++i)
            {
                frame item;
                data.frames.push_back(item);
                auto& current = data.frames.back();
                const auto attachments=data.targets->at(i);
                current.hdr={attachments.hdr,attachments.hdr_view};
                current.ldr={attachments.ldr,attachments.ldr_view};

                current.lighting=data.lighting_sets->set(i);
                current.tone=data.tone_sets->set(i);
                current.presentation=data.present_sets->set(i);

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
                for(std::uint32_t j=0;j<lighting.size();++j)
                    data.lighting_sets->write_image(i,j,lighting[j]);
                std::vector<vk::DescriptorImageInfo> tone(2);
                tone[0].sampler = data.sampler;
                tone[0].imageView = current.hdr.view;
                tone[0].imageLayout = vk::ImageLayout::eGeneral;
                tone[1].imageView = current.ldr.view;
                tone[1].imageLayout = vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<tone.size();++j)
                    data.tone_sets->write_image(i,j,tone[j]);
                std::vector<vk::DescriptorImageInfo> presented(1);
                presented[0].sampler = data.sampler;
                presented[0].imageView = current.ldr.view;
                presented[0].imageLayout = vk::ImageLayout::eGeneral;
                data.present_sets->write_image(i,0,presented[0]);
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

        }
        data.frames.clear();

        data.lighting_program.reset();
        data.tone_program.reset();
        data.present_program.reset();
        data.presentation.reset();
        if (data.present_layout) device.destroyPipelineLayout(data.present_layout);
        data.lighting_sets.reset();
        data.tone_sets.reset();
        data.present_sets.reset();
        data.targets.reset();
        if (data.sampler) device.destroySampler(data.sampler);

        data = {};
    }

    targets images(std::uint32_t image)
    {
        const auto& frame = get().frames.at(image);
        return {frame.hdr.image, frame.ldr.image};
    }

    vk::ImageView hdr_view(std::uint32_t image)
    {
        return get().frames.at(image).hdr.view;
    }

    vk::Sampler sampler()
    {
        return get().sampler;
    }

    void lighting(vk::CommandBuffer command, std::uint32_t image)
    {
        const auto& current = get().frames.at(image);
        compute(command, *get().lighting_program,
                current.lighting, enabled(effect::lighting));
    }

    void tonemap(vk::CommandBuffer command, std::uint32_t image)
    {
        const auto& current = get().frames.at(image);
        compute(command, *get().tone_program,
                current.tone, enabled(effect::tonemapping));
    }

    void present(vk::CommandBuffer command, std::uint32_t image)
    {
        const auto extent = vk::swapchain::get()->extent;
        vk::ClearValue clear{};
        clear.color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 1});
        vk::RenderPassBeginInfo info{};
        info.renderPass = get().presentation->pass();
        info.framebuffer = get().presentation->framebuffer(image);
        info.renderArea = vk::Rect2D{{0, 0}, extent};
        info.clearValueCount = 1;
        info.pClearValues = &clear;

        ::render::renderer graphics(command);
        graphics.begin_pass(info);
        graphics.viewport(
            vk::Viewport{0, 0, float(extent.width), float(extent.height), 0, 1},
            vk::Rect2D{{0, 0}, extent});
        ::render::mesh triangle{};
        triangle.vertex_count = 3;
        ::render::material surface{};
        surface.pipeline = get().present_program->handle();
        surface.layout = get().present_layout;
        surface.sets = {get().frames.at(image).presentation};
        graphics.draw(triangle, surface);
        graphics.end_pass();
    }
}
