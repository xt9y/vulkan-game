#include "clt/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>

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
