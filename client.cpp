#include "clt/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>
#include <GLFW/glfw3.h>

namespace
{
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
        settings.simulation.tick_hz = 60;
        settings.player.spawn = {0, 0, 4};
        settings.player.walk_speed = 5.0f;
        settings.player.sprint_speed = 8.0f;
        settings.player.gravity = 20.0f;
        settings.player.radius = 0.3f;
        settings.player.height = 1.8f;
        settings.view.fov = 1.04719755f;
        settings.view.sensitivity = 0.0025f;
        settings.view.eye_height = 1.65f;
        settings.view.pitch_limit = 1.55f;
        settings.view.near_plane = 0.1f;
        settings.view.far_plane = 1000.0f;
        settings.graphics.width = 1280;
        settings.graphics.height = 720;
        settings.graphics.title = "vulkan-game";
        settings.graphics.connected_title = "vulkan-game | connected";
        settings.graphics.connecting_title = "vulkan-game | connecting";
        settings.graphics.vertex_shader = "build/shaders/mesh.vert.spv";
        settings.graphics.fragment_shader = "build/shaders/mesh.frag.spv";
        settings.graphics.surface = game::options::surface_format::bgra8_srgb;
        settings.graphics.depth = game::options::depth_precision::d32;
        settings.graphics.present = game::options::presentation::fifo;
        settings.graphics.lighting = true;
        settings.graphics.tonemapping = true;
        settings.graphics.backface_culling = false;
        settings.graphics.gpu_culling = true;
        settings.graphics.gbuffer = game::options::gbuffer_precision::rgba8;
        settings.graphics.culling_shader = "build/shaders/cull.comp.spv";
        settings.graphics.lighting_shader = "build/shaders/light.comp.spv";
        settings.graphics.tonemap_shader = "build/shaders/tone.comp.spv";
        settings.graphics.fullscreen_shader = "build/shaders/fullscreen.vert.spv";
        settings.graphics.presentation_shader = "build/shaders/present.frag.spv";
        settings.graphics.exposure = 1.0f;
        settings.graphics.sun_direction = {0.5f, 1.0f, 0.3f};
        settings.graphics.sun_intensity = 3.0f;
        settings.graphics.ambient_intensity = 0.08f;
        settings.graphics.ambient_specular = 0.02f;
        settings.graphics.sky_color = {0.035f, 0.045f, 0.065f};
        settings.graphics.correction_limit = 2.0f;
        settings.graphics.correction_decay = 0.8f;
        settings.graphics.timeout_ms = 3000;
        settings.graphics.retry_ms = 250;
        settings.graphics.retry_max_ms = 1000;
        settings.graphics.heartbeat_ms = 50;
        settings.graphics.scene_request_ms = 40;
        settings.graphics.movement_ms = 16;
        settings.controls = {
            GLFW_KEY_W, GLFW_KEY_S, GLFW_KEY_A, GLFW_KEY_D,
            GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT,
            GLFW_KEY_TAB, GLFW_KEY_ESCAPE, GLFW_KEY_F6, GLFW_KEY_F7,
            true
        };
        game::options::init(argc, argv, game::options::mode::client_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

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
