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
#include <cmath>
#include <csignal>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace game_blood
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

        // User-facing blood simulation resolution. The fluid backend is
        // unchanged and shared; only the blood material profile is enabled.
        // Cover the actual walled room (about 10 metres across) instead
        // of stopping the liquid at an invisible 6.4 metre grid boundary.
        // A slightly coarser cell keeps the GPU grid cost bounded.
        constexpr std::uint32_t liquid_width=72;
        constexpr std::uint32_t liquid_height=48;
        constexpr std::uint32_t liquid_depth=72;
        constexpr float liquid_cell_size=.145f;
        constexpr std::uint32_t pressure_iterations=36;
        constexpr std::uint32_t substeps=3;
        constexpr float fluid_render_scale=.5f;
        bool continuum_initialized=false;
    }

    void release();

    void initialize(const ::scene::state& scene)
    {
        const auto extent=vk::swapchain::get()->extent;
        const auto frames=std::uint32_t(vk::swapchain::get()->images.size());
        fld::continuum::drop initial{};
        initial.center={.9f,2.75f,.75f};
        initial.radius=.68f;
        fld::continuum::options settings{};
        settings.width=liquid_width;
        settings.height=liquid_height;
        settings.depth=liquid_depth;
        settings.cell_size=liquid_cell_size;
        settings.origin={
            -0.5f*liquid_width*liquid_cell_size,
            0.f,
            -0.5f*liquid_depth*liquid_cell_size
        }; // Y is aligned below the actual floor after inspecting the scene.
        settings.gravity={0.f,-9.81f,0.f};
        settings.pressure_iterations=pressure_iterations;
        settings.substeps=substeps;
        settings.render_scale=fluid_render_scale;
        // Collider geometry is authored by server.cpp and streamed normally.
        settings.colliders.reserve(scene.collision_boxes.size());
        for(const auto& box:scene.collision_boxes)
        {
            settings.colliders.push_back({
                {box.minimum.x,box.minimum.y,box.minimum.z,0.f},
                {box.maximum.x,box.maximum.y,box.maximum.z,0.f}
            });
        }
        // The server owns the floor geometry. Place a few grid layers below
        // its upper face so the floor is voxelized as a real solid, not an
        // arbitrary boundary at the bottom of the simulation domain.
        float support=-std::numeric_limits<float>::infinity();
        for(const auto& box:settings.colliders)
        {
            if(initial.center.x>=box.minimum.x &&
               initial.center.x<=box.maximum.x &&
               initial.center.z>=box.minimum.z &&
               initial.center.z<=box.maximum.z &&
               box.maximum.y<=initial.center.y)
                support=std::max(support,box.maximum.y);
        }
        if(std::isfinite(support))
            settings.origin.y=support-4.f*liquid_cell_size;
#ifndef NDEBUG
        std::clog << "blood: support_y="
                  << (std::isfinite(support)?support:settings.origin.y)
                  << " grid_origin_y=" << settings.origin.y
                  << " colliders=" << settings.colliders.size() << '\n';
#endif
        // The former water-only front end is disabled. Blood now provides
        // material/rheology defaults and optical shaders; all simulation,
        // collision, and conservative VOF infrastructure is still reused.
        // Adjust the blood material here without editing the engine.
        fld::blood::material blood{};
        blood.substrate_drag=3.f;
        const auto paths=fld::blood::shaders("build/shaders/");
        fld::blood::init(settings,initial,paths,frames,
                         extent.width,extent.height,blood);
        continuum_initialized=true;
    }

    void create()
    {
        if(!targets.empty() || continuum_initialized)
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
        return !targets.empty() && fld::blood::active() &&
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
        if (!continuum_initialized)
        {
            const auto scene=clt::connection::snapshot();
            if (!scene) return;
            initialize(*scene);
        }
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
        const auto gbuffer=clt::scene_renderer::outputs(image);
        fld::continuum::render_input input{};
        input.width=gbuffer.extent.width;
        input.height=gbuffer.extent.height;
        input.view_projection=clt::scene_renderer::world_view_projection();
        input.depth=gbuffer.views[3];
        input.scene_color=clt::postprocess::hdr_view(image);
        input.output=target.view;
        input.sampler=clt::postprocess::sampler();
        input.color_layout=vk::ImageLayout::eGeneral;
        fld::blood::record(command,image,seconds,input);
#ifndef NDEBUG
        ++submitted;
        const float report_seconds=std::chrono::duration<float>(now-report_started).count();
        if(report_seconds>=3.f)
        {
            std::clog << "blood: submitted_fps=" << submitted/report_seconds
                      << " continuum_grid=" << liquid_width << "x"
                      << liquid_height << "x" << liquid_depth
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
        fld::blood::shutdown();
        continuum_initialized=false;
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
            game_blood::create, game_blood::release,
            game_blood::active, game_blood::output, game_blood::record
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
