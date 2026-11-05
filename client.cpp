#include "clt/app/application.hpp"
#include "clt/network/connection.hpp"
#include "clt/render/postprocess.hpp"
#include "clt/render/renderer.hpp"
#include "clt/render/scene_renderer.hpp"
#include "clt/weapon/weapon.hpp"
#include "weapons.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"

#include <fld/fld.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace game_fluid
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
        bool started=false;
        std::atomic<int> color_mode{0}; // 0 green, 1 red, 2 clear

        constexpr const char* particle_stages[11]={
            "CLEAR","PREDICT","BUILD","LAMBDA","DELTA","APPLY",
            "FINALIZE","NORMAL","CAPILLARY","VISCOSITY","VISCOSITY_APPLY"
        };
        constexpr const char* bvh_stages[8]={
            "KEYS","SORT","CLEAR","TOPOLOGY",
            "LEAVES","REFIT","OVERLAP","RAY"
        };
        // User-facing quality profile: physics resolution is independent of
        // optical resolution. Raise these values for quality after profiling.
        constexpr float particle_spacing=.064f;
        // Two smaller physics steps improve impact stability and density
        // projection at the current particle spacing.
        constexpr std::uint32_t pressure_iterations=2;
        constexpr std::uint32_t substeps=2;
        constexpr float fluid_render_scale=.5f;
        constexpr std::uint32_t fluid_render_stride=4;
    }

    void release();

    void cycle_color()
    {
        const int current=color_mode.load(std::memory_order_relaxed);
        color_mode.store((current+1)%3,std::memory_order_relaxed);
    }

    void initialize(const ::scene::state& scene)
    {
        const auto extent=vk::swapchain::get()->extent;
        const auto frames=std::uint32_t(vk::swapchain::get()->images.size());
        fld::options options{};
        // Coarse real-time physics: stable substeps and a conserved particle
        // mass for one body of liquid. Visual particle LOD is independent.
        options.max_particles=8192;
        options.spacing=particle_spacing;
        options.smoothing_radius=particle_spacing*2.67f;
        options.pressure_iterations=pressure_iterations;
        options.substeps=substeps;
        options.floor_height=0.f;
        // World collision bounds come exclusively from server.cpp::add().
        options.colliders.reserve(scene.collision_boxes.size());
        for (const auto& box : scene.collision_boxes)
        {
            options.colliders.push_back({
                {box.minimum.x,box.minimum.y,box.minimum.z,0.f},
                {box.maximum.x,box.maximum.y,box.maximum.z,0.f}
            });
        }
        fld::drop initial{};
        // Spawn nearer to the player at z=4 and in front of the props.
        initial.center={.9f,2.75f,.75f};
        initial.radius=.68f;
        fld::shader_paths paths{};
        for(std::uint32_t i=0;i<11;++i)
            paths[i]=std::string("build/shaders/fluid_")+
                      particle_stages[i]+".comp.spv";
        paths[11]="build/shaders/fluid_PREDICT_bvh.comp.spv";
        paths[12]="build/shaders/fluid_APPLY_bvh.comp.spv";
        paths[13]="build/shaders/fluid_FINALIZE_bvh.comp.spv";
        for(std::uint32_t i=0;i<options.bvh_shaders.size();++i)
            options.bvh_shaders[i]=std::string("build/shaders/bvh_")+
                                   bvh_stages[i]+".comp.spv";
        fld::system::init(options,initial,paths);
        fld::render::init({
            "build/shaders/fluid_tile_clear.comp.spv",
            "build/shaders/fluid_tile_build.comp.spv",
            "build/shaders/fluid_surface.comp.spv",
            "build/shaders/fluid_upscale.comp.spv"
        },frames,extent.width,extent.height,
          fluid_render_scale,fluid_render_stride);
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
            started=false;
        }
        catch(...)
        {
            release();
            throw;
        }
    }

    bool active()
    {
        return !targets.empty() && fld::render::active() &&
               clt::connection::get()->scene_complete.load(std::memory_order_acquire);
    }

    vk::ImageView output(std::uint32_t image)
    {
        return targets.at(image).view;
    }

    void record(vk::CommandBuffer command,std::uint32_t image)
    {
        if (targets.empty() ||
            !clt::connection::get()->scene_complete.load(std::memory_order_acquire))
            return;
        // The scene is asynchronous: initialize GPU collisions only after
        // all authoritative boxes are streamed, never before connection.
        if (!fld::system::get())
        {
            const auto scene=clt::connection::snapshot();
            if (!scene) return;
            initialize(*scene);
        }
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
        if(!started)
        {
            previous=report_started=now;
            submitted=0;
            started=true;
        }
        const auto seconds=std::chrono::duration<float>(now-previous).count();
        previous=now;
        fld::system::record(command,std::min(seconds,1.f/30.f));
        const auto gbuffer=clt::scene_renderer::outputs(image);
        fld::render::input input{};
        input.width=gbuffer.extent.width;
        input.height=gbuffer.extent.height;
        input.view_projection=clt::scene_renderer::world_view_projection();
        input.depth=gbuffer.views[3];
        input.scene_color=clt::postprocess::hdr_view(image);
        input.output=target.view;
        input.sampler=clt::postprocess::sampler();
        input.color_layout=vk::ImageLayout::eGeneral;
        // Debug appearance only: edit RGB and opacity here without
        // changing liquid density, volume, viscosity or capillary forces.
        const int tint=color_mode.load(std::memory_order_relaxed);
        input.debug_color=tint==1?
            fld::vec3{1.f,.035f,.035f}:fld::vec3{.04f,1.f,.06f};
        input.debug_opacity=tint==2?0.f:.85f;
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
        started=false;
        fld::render::shutdown();
        fld::system::shutdown();
    }
}

namespace
{
    void load_weapons()
    {
        for(auto&& gun:game_weapons::all())
            clt::weapon::register_weapon(std::move(gun));
    }

    void on_input(const clt::input::frame& frame)
    {
        static bool color_held=false;
        const bool color_key=clt::input::key_down(GLFW_KEY_F8);
        if(color_key && !color_held)game_fluid::cycle_color();
        color_held=color_key;

        // Each gun chooses its own equip key, trigger, ADS and reload input.
        static std::array<bool,GLFW_KEY_LAST+1> equip_held{};
        for(const auto& item:clt::weapon::equip_bindings())
        {
            if(item.key<0 || item.key>GLFW_KEY_LAST)continue;
            const bool down=clt::input::key_down(item.key);
            if(down && !equip_held[item.key])
                clt::weapon::equip(item.id);
            equip_held[item.key]=down;
        }
        const auto keys=clt::weapon::active_bindings();
        clt::weapon::controls(
            clt::input::mouse_down(keys.trigger_button),
            clt::input::mouse_down(keys.aim_button),
            clt::input::key_down(keys.reload_key),frame.buttons);
    }

    void stop(int)
    {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }
}

int main(int argc, char** argv)
{
    try
    {
        game::options::state settings{};
        settings.host = "127.0.0.1";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
        settings.graphics.width = 1280;
        settings.graphics.height = 720;
        settings.graphics.title = "vulkan-game";
        settings.graphics.vertex_shader = "build/shaders/mesh.vert.spv";
        settings.graphics.fragment_shader = "build/shaders/mesh.frag.spv";
        settings.graphics.surface = game::options::surface_format::bgra8_srgb;
        settings.graphics.depth = game::options::depth_precision::d32;
        settings.graphics.present = game::options::presentation::fifo;
        settings.graphics.gbuffer = game::options::gbuffer_precision::rgba8;
        settings.graphics.culling_shader = "build/shaders/cull.comp.spv";
        settings.graphics.lighting_shader = "build/shaders/light.comp.spv";
        settings.graphics.tonemap_shader = "build/shaders/tone.comp.spv";
        settings.graphics.fullscreen_shader = "build/shaders/fullscreen.vert.spv";
        settings.graphics.presentation_shader = "build/shaders/present.frag.spv";
        settings.graphics.exposure = 1.0f;
        settings.graphics.sun_direction = {0.5f, 1.0f, 0.3f};
        settings.graphics.sun_intensity = 3.0f;

        game::options::init(argc, argv, game::options::mode::client_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        load_weapons();
        clt::application::set_callbacks({on_input});
        clt::renderer::set_effect({
            game_fluid::create, game_fluid::release,
            game_fluid::active, game_fluid::output, game_fluid::record
        });
        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        clt::application::init();
        const int result = clt::application::run();
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-client: " << e.what() << '\n';
        return 1;
    }
}
