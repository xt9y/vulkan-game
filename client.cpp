#include "clt/network/connection.hpp"
#include "clt/render/postprocess.hpp"
#include "clt/render/renderer.hpp"
#include "clt/render/scene_renderer.hpp"
#include <fld/fld.hpp>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <string>
#include "clt/app/application.hpp"
#include "clt/weapon/weapon.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/weapon/metadata.hpp"

#include <GLFW/glfw3.h>
#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>

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

        constexpr const char* particle_stages[11]={
            "CLEAR","PREDICT","BUILD","LAMBDA","DELTA","APPLY",
            "FINALIZE","NORMAL","CAPILLARY","VISCOSITY","VISCOSITY_APPLY"
        };
    }

    void release();

    void initialize(const ::scene::state& scene)
    {
        const auto extent=vk::swapchain::get()->extent;
        const auto frames=std::uint32_t(vk::swapchain::get()->images.size());
            fld::options options{};
            // Double the drop radius at fixed physical particle spacing:
            // eight times the volume and 17,845 SPH particles (was 2,205).
            // Maintain resolution rather than inflating the particle radius.
            options.max_particles=20000;
            options.spacing=.042f;
            options.smoothing_radius=.112f;
            options.pressure_iterations=4;
            options.substeps=2;
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
            for(std::uint32_t i=0;i<paths.size();++i)
                paths[i]=std::string("build/shaders/fluid_")+
                          particle_stages[i]+".comp.spv";
            fld::system::init(options,initial,paths);
            fld::render::init({
                "build/shaders/fluid_tile_clear.comp.spv",
                "build/shaders/fluid_tile_build.comp.spv",
                "build/shaders/fluid_surface.comp.spv"
            },frames,extent.width,extent.height);
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
        // Client-only resources and presentation. IDs must match server.cpp.
        using motion = clt::weapon::motion;
        clt::weapon::definition glock;
        glock.rules.id = 1;
        glock.rules.magazine_capacity = 15;
        glock.rules.initial_reserve = 90;
        glock.rules.fire_interval = .15;
        glock.rules.range = 200;
        glock.rules.reload = weapon::load_timing(
            "assets/glock/metadata.json", "Glock_Reload", "Glock_ReloadEmpty", 20, 20
        );
        glock.model = "assets/glock/glock.glb";
        glock.casing_model = "assets/props/casing.glb";
        glock.camera_joint = "camera";
        glock.muzzle_joint = "Muzzle";
        glock.ejection_joint = "EjectionPort";
        glock.slide_joint = "Slide";
        glock.animations = {
            {motion::idle, {"Glock_Idle", 1, true}},
            {motion::walk, {"Glock_Walk", 1, true}},
            {motion::draw, {"Glock_Draw", 1, false}},
            {motion::reload, {"Glock_Reload", 1, false}},
            {motion::reload_empty, {"Glock_ReloadEmpty", 1, false}},
            {motion::ads_idle, {"Glock_ADS_Idle", 1, true}},
            {motion::ads_walk, {"Glock_ADS_Walk", 1, true}},
            {motion::ads_in, {"Glock_ADS_In", 3, false}},
            {motion::ads_out, {"Glock_ADS_Out", 3, false}},
            {motion::fire1, {"Glock_Fire1", 1.5f, false}},
            {motion::fire2, {"Glock_Fire2", 1.5f, false}},
            {motion::fire3, {"Glock_Fire3", 1.5f, false}},
            {motion::ads_fire1, {"Glock_ADS_Fire1", 1.5f, false}},
            {motion::ads_fire2, {"Glock_ADS_Fire2", 1.5f, false}},
            {motion::ads_fire3, {"Glock_ADS_Fire3", 1.5f, false}}
        };
        glock.fov = 1.22173048f;
        glock.ads_fov = glock.fov - .08f;
        glock.ads_zoom = .08f;
        glock.slide_offset = .025f;
        glock.recoil = .022f;
        glock.hip_kick = .015f;
        glock.ads_kick = .009f;
        glock.casing_impulse = {1.6f, 1.5f, .35f};
        glock.casing_lifetime = 9;
        glock.max_casings = 48;
        glock.flash_duration = .035f;
        glock.flash_scale = {.035f, .045f, .035f};
        glock.impact_radius = .009f;
        glock.impact_lifetime = 120;
        glock.max_impacts = 192;
        // Register more IDs here; models/materials are cached and uploaded once.
        clt::weapon::register_weapon(std::move(glock));
    }

    void on_input(const clt::input::frame& frame)
    {
        static bool equipment_held[9]{};
        for (int slot = 0; slot < 9; ++slot)
        {
            const bool down = clt::input::key_down(GLFW_KEY_1 + slot);
            if (down && !equipment_held[slot])
            {
                clt::weapon::equip(static_cast<std::uint32_t>(slot + 1));
            }
            equipment_held[slot] = down;
        }

        clt::weapon::controls(
            clt::input::mouse_down(GLFW_MOUSE_BUTTON_LEFT),
            clt::input::mouse_down(GLFW_MOUSE_BUTTON_RIGHT),
            clt::input::key_down(GLFW_KEY_R),
            frame.buttons
        );
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
