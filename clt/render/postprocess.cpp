#include "postprocess.hpp"
#include "srv/game/options.hpp"
#include "scene_renderer.hpp"
#include "clt/view/camera.hpp"
#include <render/renderer.hpp>
#include <render/pipeline.hpp>
#include <gpu/compute.hpp>
#include <gpu/descriptors.hpp>
#include <gpu/resource.hpp>
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
            gpu::owned_image indirect;
            vk::ImageView indirect_view{};
            vk::DescriptorSet gi{};
            vk::DescriptorSet lighting{};
            vk::DescriptorSet tone{};
            vk::DescriptorSet presentation{};
        };

        struct state {
            std::vector<frame> frames;
            vk::Sampler sampler{};
            gpu::owned_buffer lights;
            std::unique_ptr<gpu::descriptors> gi_sets;
            std::unique_ptr<gpu::descriptors> lighting_sets;
            std::unique_ptr<gpu::descriptors> tone_sets;
            std::unique_ptr<gpu::descriptors> present_sets;
            std::unique_ptr<::render::postprocess_targets> targets;
            std::unique_ptr<::render::presentation_targets> presentation;
            vk::PipelineLayout present_layout{};
            std::unique_ptr<gpu::compute> gi_program;
            std::unique_ptr<gpu::compute> lighting_program;
            std::unique_ptr<gpu::compute> tone_program;
            std::unique_ptr<::render::pipeline> present_program;
        };

        state& get()
        {
            static state data;
            return data;
        }

        std::atomic<float> exposure_value{};

        struct alignas(16) point_light {
            std::array<float,4> position_range{};
            std::array<float,4> color_intensity{};
        };
        static_assert(sizeof(point_light)==32);

        struct push {
            std::int32_t enabled;
            float exposure;
            std::array<float,2> padding{};
            std::array<float,4> eye_near{};
            std::array<float,4> forward_far{};
            std::array<float,4> right_tan{};
            std::array<float,4> up_tan{};
            std::array<float,4> sun{};
            std::array<float,4> ambient_count{};
            std::array<float,4> sky{};
        };
        static_assert(sizeof(push)==128);

        std::vector<vk::DescriptorSetLayoutBinding> bindings(
            std::uint32_t sampled,std::uint32_t storage,vk::ShaderStageFlags stages,
            std::uint32_t buffers=0)
        {
            std::vector<vk::DescriptorSetLayoutBinding> result(sampled+storage+buffers);
            for(std::uint32_t i=0;i<result.size();++i) {
                auto& binding=result[i];
                binding.binding=i;
                binding.descriptorCount=1;
                binding.stageFlags=stages;
                binding.descriptorType=i<sampled
                    ? vk::DescriptorType::eCombinedImageSampler
                    : i<sampled+storage ? vk::DescriptorType::eStorageImage
                                        : vk::DescriptorType::eStorageBuffer;
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

        void compute(vk::CommandBuffer command,const gpu::compute& program,
                     vk::DescriptorSet set,const camera::frame& frame,
                     bool half=false)
        {
            const auto extent=vk::swapchain::get()->extent;
            const auto& view=frame.pose;
            const float sy=std::sin(view.yaw),cy=std::cos(view.yaw);
            const float sp=std::sin(view.pitch),cp=std::cos(view.pitch);
            const float tan_half=std::tan(view.fov*0.5f);
            const float aspect=float(extent.width)/float(extent.height);
            const auto& position=frame.eye;
            const auto& lighting=game::options::get()->graphics;
            const push controls{
                1,exposure_value.load(std::memory_order_relaxed),{},
                {position.x,position.y,position.z,0.1f},
                {sy*cp,sp,-cy*cp,1000.0f},
                {cy,0,sy,tan_half*aspect},
                {-sy*sp,cp,cy*sp,tan_half},
                {lighting.sun_direction.x,lighting.sun_direction.y,
                 lighting.sun_direction.z,lighting.sun_intensity},
                {0.01f,0.015f,0.02f,
                 float(std::min<std::size_t>(lighting.point_lights.size(),64))},
                {0.035f,0.045f,0.065f,1.0f}
            };
            const auto width=half?(extent.width+1)/2:extent.width;
            const auto height=half?(extent.height+1)/2:extent.height;
            program.dispatch(command,(width+7)/8,(height+7)/8,1,
                             {set},&controls,sizeof(controls));
        }
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
            data.gi_sets=std::make_unique<gpu::descriptors>();
            data.gi_sets->create(bindings(4,1,vk::ShaderStageFlagBits::eCompute,1),count);
            data.lighting_sets=std::make_unique<gpu::descriptors>();
            data.lighting_sets->create(bindings(5,1,vk::ShaderStageFlagBits::eCompute,1),count);
            data.tone_sets=std::make_unique<gpu::descriptors>();
            data.tone_sets->create(bindings(1,1,vk::ShaderStageFlagBits::eCompute),count);
            data.present_sets=std::make_unique<gpu::descriptors>();
            data.present_sets->create(bindings(1,0,vk::ShaderStageFlagBits::eFragment),count);
            data.present_layout=pipeline_layout(data.present_sets->layout(),false);
            data.gi_program=std::make_unique<gpu::compute>();
            data.gi_program->create("build/shaders/gi.comp.spv",
                                    {data.gi_sets->layout()},sizeof(push));
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

            // Static light data is set by client.cpp. The scene does not yet
            // transmit dynamic lights; retain a valid descriptor if empty.
            const auto& source_lights=game::options::get()->graphics.point_lights;
            std::vector<point_light> lights(std::max<std::size_t>(1,source_lights.size()));
            for(std::size_t i=0;i<source_lights.size();++i)
            {
                const auto& src=source_lights[i];
                lights[i]={{src.position.x,src.position.y,src.position.z,src.range},
                           {src.color.x,src.color.y,src.color.z,src.intensity}};
            }
            data.lights=gpu::create_buffer(lights.size()*sizeof(point_light),
                vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent);
            vk::buffer::write(data.lights,lights.data(),lights.size()*sizeof(point_light));

            data.frames.reserve(count);
            for(std::uint32_t i=0;i<count;++i)
            {
                data.frames.emplace_back();
                auto& current=data.frames.back();
                const auto attachments=data.targets->at(i);
                current.hdr={attachments.hdr,attachments.hdr_view};
                current.ldr={attachments.ldr,attachments.ldr_view};

                vk::ImageCreateInfo gi_image{};
                gi_image.imageType=vk::ImageType::e2D;
                gi_image.format=vk::Format::eR16G16B16A16Sfloat;
                gi_image.extent=vk::Extent3D{
                    (vk::swapchain::get()->extent.width+1)/2,
                    (vk::swapchain::get()->extent.height+1)/2,1};
                gi_image.mipLevels=1;
                gi_image.arrayLayers=1;
                gi_image.samples=vk::SampleCountFlagBits::e1;
                gi_image.tiling=vk::ImageTiling::eOptimal;
                gi_image.usage=vk::ImageUsageFlagBits::eStorage |
                               vk::ImageUsageFlagBits::eSampled;
                current.indirect=gpu::create_image(
                    gi_image,vk::MemoryPropertyFlagBits::eDeviceLocal);
                vk::ImageViewCreateInfo view{};
                view.image=current.indirect.handle;
                view.viewType=vk::ImageViewType::e2D;
                view.format=gi_image.format;
                view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
                current.indirect_view=vk::image_view::create(view);

                current.gi=data.gi_sets->set(i);
                current.lighting=data.lighting_sets->set(i);
                current.tone=data.tone_sets->set(i);
                current.presentation=data.present_sets->set(i);

                const auto g = scene_renderer::outputs(i);
                std::vector<vk::DescriptorImageInfo> gi(5);
                for(std::size_t j=0;j<g.views.size();++j)
                {
                    gi[j].sampler=data.sampler;
                    gi[j].imageView=g.views[j];
                    gi[j].imageLayout=j==3
                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                        : vk::ImageLayout::eShaderReadOnlyOptimal;
                }
                gi[4].imageView=current.indirect_view;
                gi[4].imageLayout=vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<gi.size();++j)
                    data.gi_sets->write_image(i,j,gi[j]);
                data.gi_sets->write_buffer(i,5,
                    vk::buffer::descriptor(data.lights));

                std::vector<vk::DescriptorImageInfo> lighting(6);
                for(std::size_t j=0;j<g.views.size();++j)
                    lighting[j]=gi[j];
                lighting[4].sampler=data.sampler;
                lighting[4].imageView=current.indirect_view;
                lighting[4].imageLayout=vk::ImageLayout::eGeneral;
                lighting[5].imageView=current.hdr.view;
                lighting[5].imageLayout=vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<lighting.size();++j)
                    data.lighting_sets->write_image(i,j,lighting[j]);
                data.lighting_sets->write_buffer(i,6,
                    vk::buffer::descriptor(data.lights));
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
        // Image views are destroyed before their RAII image allocations.
        for(auto& frame:data.frames)
        {
            if(frame.indirect_view)
                vk::image_view::destroy(frame.indirect_view);
            frame.indirect_view=nullptr;
        }
        data.frames.clear();

        data.gi_program.reset();
        data.lighting_program.reset();
        data.tone_program.reset();
        data.present_program.reset();
        data.presentation.reset();
        if (data.present_layout) device.destroyPipelineLayout(data.present_layout);
        data.gi_sets.reset();
        data.lighting_sets.reset();
        data.tone_sets.reset();
        data.present_sets.reset();
        data.targets.reset();
        data.lights.reset();
        if (data.sampler) device.destroySampler(data.sampler);

        data = {};
    }

    targets images(std::uint32_t image)
    {
        const auto& frame = get().frames.at(image);
        return {frame.indirect.handle,frame.hdr.image,frame.ldr.image};
    }

    void gi(vk::CommandBuffer command,std::uint32_t image,
            const camera::frame& frame)
    {
        const auto& current=get().frames.at(image);
        compute(command,*get().gi_program,current.gi,frame,true);
    }

    void lighting(vk::CommandBuffer command, std::uint32_t image,
                  const camera::frame& frame)
    {
        const auto& current = get().frames.at(image);
        compute(command,*get().lighting_program,current.lighting,frame);
    }

    void tonemap(vk::CommandBuffer command, std::uint32_t image,
                 const camera::frame& frame)
    {
        const auto& current = get().frames.at(image);
        compute(command,*get().tone_program,current.tone,frame);
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
