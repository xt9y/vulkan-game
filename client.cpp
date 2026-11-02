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
