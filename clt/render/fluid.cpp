#include "fluid.hpp"
#include "postprocess.hpp"
#include "scene_renderer.hpp"
#include <fld/fld.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace clt::fluid
{
    namespace
    {
        using clock=std::chrono::steady_clock;
        struct target
        {
            vk::image::resource image{};
            vk::ImageView view{};
            bool initialized=false;
        };
        std::vector<target> targets;
        clock::time_point previous{},report_started{};
        std::uint32_t submitted=0;

        constexpr const char* particle_stages[11]={
            "CLEAR","PREDICT","BUILD","LAMBDA","DELTA","APPLY",
            "FINALIZE","NORMAL","CAPILLARY","VISCOSITY","VISCOSITY_APPLY"
        };
    }

    void create()
    {
        if(!targets.empty() || fld::system::get())
            throw std::logic_error("Fluid pass already initialized");
        const auto extent=vk::swapchain::get()->extent;
        const auto frames=std::uint32_t(vk::swapchain::get()->images.size());
        if(!extent.width || !extent.height || !frames)
            throw std::runtime_error("Invalid fluid output dimensions");
        try
        {
            fld::options options{};
            // A coherent liquid volume, not a handful of rendered beads.
            // 0.34 m / 0.042 m generates 2,205 physical SPH particles.
            options.max_particles=2800;
            options.spacing=.042f;
            options.smoothing_radius=.112f;
            options.pressure_iterations=4;
            options.substeps=2;
            options.floor_height=0.f;
            const auto solid=[](float x0,float y0,float z0,
                                float x1,float y1,float z1)
            {
                return fld::box{
                    {x0,y0,z0,0.f},{x1,y1,z1,0.f}
                };
            };
            // Match the real server test scene: four tall walls, three
            // half-metre boxes; the floor is the solver's y=0 plane.
            options.colliders={
                solid( 4.66f,0.f,-5.f, 5.34f,4.f,5.f),
                solid(-5.34f,0.f,-5.f,-4.66f,4.f,5.f),
                solid(-5.f,0.f, 4.66f,5.f,4.f,5.34f),
                solid(-5.f,0.f,-5.34f,5.f,4.f,-4.66f),
                solid(-2.5f,0.f,-.5f,-1.5f,1.f,.5f),
                solid(-.5f,0.f,-.5f,.5f,1.f,.5f),
                solid(1.5f,0.f,-.5f,2.5f,1.f,.5f)
            };
            fld::drop initial{};
            // Spawn nearer to the player at z=4 and in front of the props.
            initial.center={1.15f,2.9f,1.5f};
            initial.radius=.34f;
            fld::shader_paths paths{};
            for(std::uint32_t i=0;i<paths.size();++i)
                paths[i]=std::string("build/shaders/fluid_")+
                          particle_stages[i]+".comp.spv";
            fld::system::init(options,initial,paths);
            fld::render::init({
                "build/shaders/fluid_tile_clear.comp.spv",
                "build/shaders/fluid_tile_build.comp.spv",
                "build/shaders/fluid_surface.comp.spv"
            },frames,extent.width,extent.height);
            targets.resize(frames);
            for(auto &target:targets)
            {
                vk::ImageCreateInfo info{};
                info.imageType=vk::ImageType::e2D;
                info.format=vk::Format::eR16G16B16A16Sfloat;
                info.extent=vk::Extent3D{extent.width,extent.height,1};
                info.mipLevels=1;
                info.arrayLayers=1;
                info.samples=vk::SampleCountFlagBits::e1;
                info.tiling=vk::ImageTiling::eOptimal;
                info.usage=vk::ImageUsageFlagBits::eStorage |
                           vk::ImageUsageFlagBits::eSampled;
                target.image=vk::image::create(info,
                    vk::MemoryPropertyFlagBits::eDeviceLocal);
                vk::ImageViewCreateInfo view{};
                view.image=target.image.handle;
                view.viewType=vk::ImageViewType::e2D;
                view.format=info.format;
                view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
                target.view=vk::image_view::create(view);
            }
            previous=report_started=clock::now();
            submitted=0;
        }
        catch(...)
        {
            release();
            throw;
        }
    }

    bool active()
    {
        return !targets.empty() && fld::render::active();
    }

    vk::ImageView output(std::uint32_t image)
    {
        return targets.at(image).view;
    }

    void record(vk::CommandBuffer command,std::uint32_t image)
    {
        if(!active())return;
        auto &target=targets.at(image);
        if(!target.initialized)
        {
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout=vk::ImageLayout::eUndefined;
            barrier.newLayout=vk::ImageLayout::eGeneral;
            barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=target.image.handle;
            barrier.subresourceRange={
                vk::ImageAspectFlagBits::eColor,0,1,0,1
            };
            barrier.dstAccessMask=vk::AccessFlagBits::eShaderWrite;
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                    vk::PipelineStageFlagBits::eComputeShader,
                                    {},{},{},{barrier});
            target.initialized=true;
        }
        const auto now=clock::now();
        const auto seconds=std::chrono::duration<float>(now-previous).count();
        previous=now;
        fld::system::record(command,std::min(seconds,1.f/30.f));
        const auto gbuffer=scene_renderer::outputs(image);
        fld::render::input input{};
        input.width=gbuffer.extent.width;
        input.height=gbuffer.extent.height;
        input.view_projection=scene_renderer::world_view_projection();
        input.depth=gbuffer.views[3];
        input.scene_color=postprocess::hdr_view(image);
        input.output=target.view;
        input.sampler=postprocess::sampler();
        input.color_layout=vk::ImageLayout::eGeneral;
        fld::render::record(command,image,input);
#ifndef NDEBUG
        ++submitted;
        const float report_seconds=std::chrono::duration<float>(now-report_started).count();
        if(report_seconds>=3.f)
        {
            std::clog << "fluid: submitted_fps=" << submitted/report_seconds
                      << " particles=" << fld::system::particle_count()
                      << " (GPU frame time not measured)" << std::endl;
            submitted=0;
            report_started=now;
        }
#endif
    }

    void release()
    {
        for(auto &target:targets)
        {
            if(target.view)vk::image_view::destroy(target.view);
            if(target.image.handle)vk::image::destroy(target.image);
        }
        targets.clear();
        fld::render::shutdown();
        fld::system::shutdown();
    }
}
