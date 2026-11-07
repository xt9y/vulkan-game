#include "postprocess.hpp"
#include "srv/game/options.hpp"
#include "scene_renderer.hpp"
#include "clt/network/connection.hpp"
#include "clt/view/camera.hpp"
#include "util/math/quaternion.hpp"
#include <render/renderer.hpp>
#include <render/pipeline.hpp>
#include <gpu/compute.hpp>
#include <gpu/descriptors.hpp>
#include <gpu/resource.hpp>
#include <render/postprocess.hpp>
#include <memory>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <limits>

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
            vk::DescriptorSet cloud{};
            vk::DescriptorSet gi{};
            vk::DescriptorSet lighting{};
            vk::DescriptorSet tone{};
            vk::DescriptorSet presentation{};
        };

        struct state {
            std::vector<frame> frames;
            vk::Sampler sampler{};
            gpu::owned_buffer lights;
            gpu::owned_buffer points;
            gpu::owned_buffer triangles;
            gpu::owned_image grid;
            vk::ImageView grid_view{};
            std::shared_ptr<const ::scene::state> snapshot;
            std::uint32_t light_count=0, point_count=0, vertex_count=0;
            bool dirty=true;
            std::unique_ptr<gpu::descriptors> cloud_sets;
            std::unique_ptr<gpu::descriptors> gi_sets;
            std::unique_ptr<gpu::descriptors> lighting_sets;
            std::unique_ptr<gpu::descriptors> tone_sets;
            std::unique_ptr<gpu::descriptors> present_sets;
            std::unique_ptr<::render::postprocess_targets> targets;
            std::unique_ptr<::render::presentation_targets> presentation;
            vk::PipelineLayout present_layout{};
            std::unique_ptr<gpu::compute> cloud_program;
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
        std::atomic<bool> draw_probes=false;
        constexpr std::uint32_t grid_width=32,grid_height=22,grid_depth=36;
        constexpr float grid_spacing=0.2f;
        const vk::util::math::vec3 grid_origin{-3.2f,-0.2f,-3.6f};

        struct alignas(16) point_light {
            std::array<float,4> position_range{};
            std::array<float,4> color_intensity{};
        };
        static_assert(sizeof(point_light)==32);
        struct probe_point {
            std::array<float,4> position{};
            std::array<float,4> normal{};
            std::array<float,4> radiance{};
        };
        static_assert(sizeof(probe_point)==48);
        struct cloud_push {
            std::uint32_t points,vertices,lights,padding=0;
        };
        struct grid_push {
            std::uint32_t points,vertices;
            std::array<float,2> padding{};
            std::array<float,4> origin_spacing{};
        };
        static_assert(sizeof(grid_push)==32);

        struct push {
            float exposure;
            std::array<float,3> padding{};
            std::array<float,4> eye_near{};
            std::array<float,4> forward_far{};
            std::array<float,4> right_tan{};
            std::array<float,4> up_tan{};
            std::array<float,4> light_count{};
            std::array<float,4> sky{};
        };
        static_assert(sizeof(push)==112);

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

        vk::PipelineLayout pipeline_layout(vk::DescriptorSetLayout set)
        {
            vk::PipelineLayoutCreateInfo info{};
            info.setLayoutCount=1;
            info.pSetLayouts=&set;
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
            const push controls{
                exposure_value.load(std::memory_order_relaxed),{},
                {position.x,position.y,position.z,0.1f},
                {sy*cp,sp,-cy*cp,1000.0f},
                {cy,0,sy,tan_half*aspect},
                {-sy*sp,cp,cy*sp,tan_half},
                {float(get().light_count),draw_probes.load(std::memory_order_relaxed)?1.0f:0.0f,
                 float(get().vertex_count),0.0f},
                {grid_origin.x,grid_origin.y,grid_origin.z,grid_spacing}
            };
            const auto width=half?(extent.width+1)/2:extent.width;
            const auto height=half?(extent.height+1)/2:extent.height;
            program.dispatch(command,(width+7)/8,(height+7)/8,1,
                             {set},&controls,sizeof(controls));
        }
    }

    void toggle_probes()
    {
        draw_probes.store(!draw_probes.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }

    bool needs_update()
    {
        return get().dirty;
    }

    void updated()
    {
        get().dirty=false;
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
            data.cloud_sets=std::make_unique<gpu::descriptors>();
            data.cloud_sets->create(bindings(0,0,vk::ShaderStageFlagBits::eCompute,3),count);
            data.gi_sets=std::make_unique<gpu::descriptors>();
            data.gi_sets->create(bindings(0,1,vk::ShaderStageFlagBits::eCompute,2),count);
            data.lighting_sets=std::make_unique<gpu::descriptors>();
            data.lighting_sets->create(bindings(5,1,vk::ShaderStageFlagBits::eCompute,2),count);
            data.tone_sets=std::make_unique<gpu::descriptors>();
            data.tone_sets->create(bindings(1,1,vk::ShaderStageFlagBits::eCompute),count);
            data.present_sets=std::make_unique<gpu::descriptors>();
            data.present_sets->create(bindings(1,0,vk::ShaderStageFlagBits::eFragment),count);
            data.present_layout=pipeline_layout(data.present_sets->layout());
            data.cloud_program=std::make_unique<gpu::compute>();
            data.cloud_program->create("build/shaders/cloud.comp.spv",
                                       {data.cloud_sets->layout()},sizeof(cloud_push));
            data.gi_program=std::make_unique<gpu::compute>();
            data.gi_program->create("build/shaders/gi.comp.spv",
                                    {data.gi_sets->layout()},sizeof(grid_push));
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

            const auto storage=vk::BufferUsageFlagBits::eStorageBuffer;
            const auto memory=vk::MemoryPropertyFlagBits::eHostVisible |
                              vk::MemoryPropertyFlagBits::eHostCoherent;
            data.lights=gpu::create_buffer(64*sizeof(point_light),storage,memory);
            data.points=gpu::create_buffer(sizeof(probe_point),storage,memory);
            data.triangles=gpu::create_buffer(3*sizeof(std::array<float,4>),storage,memory);

            vk::ImageCreateInfo grid_info{};
            grid_info.imageType=vk::ImageType::e3D;
            grid_info.format=vk::Format::eR16G16B16A16Sfloat;
            grid_info.extent={grid_width,grid_height,grid_depth};
            grid_info.mipLevels=1;
            grid_info.arrayLayers=1;
            grid_info.samples=vk::SampleCountFlagBits::e1;
            grid_info.tiling=vk::ImageTiling::eOptimal;
            grid_info.usage=vk::ImageUsageFlagBits::eStorage |
                            vk::ImageUsageFlagBits::eSampled;
            data.grid=gpu::create_image(grid_info,vk::MemoryPropertyFlagBits::eDeviceLocal);
            vk::ImageViewCreateInfo grid_view{};
            grid_view.image=data.grid.handle;
            grid_view.viewType=vk::ImageViewType::e3D;
            grid_view.format=grid_info.format;
            grid_view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
            data.grid_view=vk::image_view::create(grid_view);

            data.frames.reserve(count);
            for(std::uint32_t i=0;i<count;++i)
            {
                data.frames.emplace_back();
                auto& current=data.frames.back();
                const auto attachments=data.targets->at(i);
                current.hdr={attachments.hdr,attachments.hdr_view};
                current.ldr={attachments.ldr,attachments.ldr_view};

                current.cloud=data.cloud_sets->set(i);
                current.gi=data.gi_sets->set(i);
                current.lighting=data.lighting_sets->set(i);
                current.tone=data.tone_sets->set(i);
                current.presentation=data.present_sets->set(i);

                const auto g = scene_renderer::outputs(i);
                vk::DescriptorImageInfo grid_storage{};
                grid_storage.imageView=data.grid_view;
                grid_storage.imageLayout=vk::ImageLayout::eGeneral;
                data.gi_sets->write_image(i,0,grid_storage);
                data.cloud_sets->write_buffer(i,0,vk::buffer::descriptor(data.points));
                data.cloud_sets->write_buffer(i,1,vk::buffer::descriptor(data.triangles));
                data.cloud_sets->write_buffer(i,2,vk::buffer::descriptor(data.lights));
                data.gi_sets->write_buffer(i,1,vk::buffer::descriptor(data.points));
                data.gi_sets->write_buffer(i,2,vk::buffer::descriptor(data.triangles));

                std::array<vk::DescriptorImageInfo,6> lighting{};
                for(std::size_t j=0;j<g.views.size();++j)
                {
                    lighting[j].sampler=data.sampler;
                    lighting[j].imageView=g.views[j];
                    lighting[j].imageLayout=j==3
                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                        : vk::ImageLayout::eShaderReadOnlyOptimal;
                }
                lighting[4].sampler=data.sampler;
                lighting[4].imageView=data.grid_view;
                lighting[4].imageLayout=vk::ImageLayout::eGeneral;
                lighting[5].imageView=current.hdr.view;
                lighting[5].imageLayout=vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<lighting.size();++j)
                    data.lighting_sets->write_image(i,j,lighting[j]);
                data.lighting_sets->write_buffer(i,6,vk::buffer::descriptor(data.lights));
                data.lighting_sets->write_buffer(i,7,vk::buffer::descriptor(data.triangles));
                std::array<vk::DescriptorImageInfo,2> tone{};
                tone[0].sampler = data.sampler;
                tone[0].imageView = current.hdr.view;
                tone[0].imageLayout = vk::ImageLayout::eGeneral;
                tone[1].imageView = current.ldr.view;
                tone[1].imageLayout = vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<tone.size();++j)
                    data.tone_sets->write_image(i,j,tone[j]);
                std::array<vk::DescriptorImageInfo,1> presented{};
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

    // Only called after all previous GPU frames are idle.
    void synchronize_lights()
    {
        auto& data=get();
        const auto source=connection::snapshot();
        const auto count=source?source->lights.size():0;
        if(count>64)
            throw std::runtime_error("Scene exceeds 64 point lights");
        if(count)
        {
            std::array<point_light,64> payload{};
            for(std::size_t i=0;i<count;++i)
            {
                const auto& light=source->lights[i];
                payload[i]={{light.position.x,light.position.y,light.position.z,light.radius},
                            {light.color.x,light.color.y,light.color.z,light.strength}};
            }
            vk::buffer::write(data.lights,payload.data(),count*sizeof(point_light));
        }
        data.light_count=static_cast<std::uint32_t>(count);
    }

    void release()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        data.frames.clear();
        data.cloud_program.reset();
        data.gi_program.reset();
        data.lighting_program.reset();
        data.tone_program.reset();
        data.present_program.reset();
        data.presentation.reset();
        if (data.present_layout) device.destroyPipelineLayout(data.present_layout);
        data.cloud_sets.reset();
        data.gi_sets.reset();
        data.lighting_sets.reset();
        data.tone_sets.reset();
        data.present_sets.reset();
        data.targets.reset();
        data.lights.reset();
        data.points.reset();
        data.triangles.reset();
        if(data.grid_view)vk::image_view::destroy(data.grid_view);
        data.grid_view={};
        data.grid.reset();
        if (data.sampler) device.destroySampler(data.sampler);

        data = {};
    }

    targets images(std::uint32_t image)
    {
        const auto& frame = get().frames.at(image);
        return {get().grid.handle,get().points.handle,frame.hdr.image,frame.ldr.image};
    }

    void cloud(vk::CommandBuffer command,std::uint32_t image)
    {
        if(!get().dirty || !get().point_count)return;
        const cloud_push push{get().point_count,get().vertex_count,get().light_count,0};
        get().cloud_program->dispatch(command,(get().point_count+63)/64,1,1,
                                      {get().frames.at(image).cloud},&push,sizeof(push));
    }

    void gi(vk::CommandBuffer command,std::uint32_t image)
    {
        if(!get().dirty)return;
        const grid_push push{
            get().point_count,get().vertex_count,{},
            {grid_origin.x,grid_origin.y,grid_origin.z,grid_spacing}
        };
        get().gi_program->dispatch(command,
            (grid_width+3)/4,(grid_height+3)/4,(grid_depth+3)/4,
            {get().frames.at(image).gi},&push,sizeof(push));
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
