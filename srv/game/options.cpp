#include "options.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace game::options
{
    state* get()
    {
        static state data;
        return &data;
    }

    void init(int argc, char** argv, mode selected_mode, state configured)
    {
        state result = std::move(configured);
        result.mode = selected_mode;
        bool host_specified = false;
        bool bind_specified = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string option = argv[i];

            if (option == "--help")
            {
                result.help = true;
                continue;
            }

            if (option == "--headless")
            {
                result.headless = true;
                continue;
            }

            if (option != "--port" && option != "--duration" && option != "--host" &&
                option != "--bind")
            {
                throw std::invalid_argument("Unknown option: " + option);
            }

            if (++i == argc)
            {
                throw std::invalid_argument("Missing value for " + option);
            }

            const std::string value = argv[i];
            std::size_t end = 0;

            if (option == "--port")
            {
                const auto port = std::stoul(value, &end);

                if (end != value.size() || port == 0 || port > 65535 || value.front() == '-')
                {
                    throw std::invalid_argument("Port must be in 1..65535");
                }

                result.port = static_cast<std::uint16_t>(port);
            }
            else if (option == "--duration")
            {
                result.duration = std::stod(value, &end);

                if (end != value.size() || !std::isfinite(result.duration) || result.duration <= 0)
                {
                    throw std::invalid_argument("Duration must be a positive number of seconds");
                }
            }
            else if (option == "--host")
            {
                result.host = value;
                host_specified = true;
            }
            else
            {
                result.bind_host = value;
                bind_specified = true;
            }
        }

        if (!result.help)
        {
            if (result.mode == mode::server_only && result.headless)
            {
                throw std::invalid_argument("--headless requires a client mode");
            }

            if (result.mode != mode::client_only && host_specified)
            {
                throw std::invalid_argument(
                    "--host is only supported by vulkan-client"
                );
            }

            if (result.mode == mode::client_only && bind_specified)
            {
                throw std::invalid_argument("--bind is only supported by vulkan-server");
            }
        }

        if (result.port == 0)
            throw std::invalid_argument("Game must set a UDP port");

        const auto& p = result.player;
        const auto& s = result.simulation;
        if (s.tick_hz == 0 || p.walk_speed <= 0 || p.sprint_speed <= 0 ||
            p.gravity <= 0 || p.radius <= 0 || p.height <= 0)
            throw std::invalid_argument("Game must configure movement and tick rate");

        if (result.mode == mode::server_only)
        {
            if (result.bind_host.empty() || s.max_clients == 0 || s.timeout_seconds == 0 ||
                s.receive_packets == 0 || s.receive_ms == 0 ||
                s.fragments_per_tick == 0 || s.fragments_ms == 0 ||
                s.inputs_per_tick == 0 || s.animation_period == 0 ||
                s.animations_per_tick == 0 || s.generation_commit_ticks == 0 ||
                s.terrain_scale <= 0 || s.detail_scale <= 0 ||
                s.terrain_amplitude < 0 || s.detail_amplitude < 0)
                throw std::invalid_argument("Server configuration is incomplete");
        }
        else
        {
            const auto& g = result.graphics;
            const auto& c = result.view;
            if (result.host.empty() || g.width <= 0 || g.height <= 0 ||
                g.title.empty() || g.connected_title.empty() ||
                g.connecting_title.empty() || g.vertex_shader.empty() ||
                g.fragment_shader.empty() ||
                g.gbuffer == gbuffer_precision::unspecified ||
                g.surface == surface_format::unspecified ||
                g.depth == depth_precision::unspecified ||
                g.present == presentation::unspecified ||
                g.culling_shader.empty() ||
                g.lighting_shader.empty() || g.tonemap_shader.empty() ||
                g.fullscreen_shader.empty() || g.presentation_shader.empty() ||
                c.fov <= 0 ||
                c.sensitivity <= 0 || c.eye_height <= 0 ||
                c.pitch_limit <= 0 || c.near_plane <= 0 ||
                c.far_plane <= c.near_plane || g.exposure < 0 ||
                g.correction_limit <= 0 || g.correction_decay <= 0 ||
                g.sun_intensity < 0 || g.ambient_intensity < 0 ||
                g.ambient_specular < 0 ||
                (g.sun_direction.x == 0 && g.sun_direction.y == 0 &&
                 g.sun_direction.z == 0) ||
                g.correction_decay >= 1 || g.timeout_ms == 0 ||
                g.retry_ms == 0 || g.retry_max_ms < g.retry_ms ||
                g.heartbeat_ms == 0 || g.scene_request_ms == 0 ||
                g.movement_ms == 0)
                throw std::invalid_argument("Client configuration is incomplete");
        }
        *get() = std::move(result);
    }

    void print_help()
    {
        const bool server = get()->mode == mode::server_only;
        std::cout << (server ? "Usage: vulkan-server [OPTIONS]\n\n"
                            : "Usage: vulkan-client [OPTIONS]\n\n")
                  << "Options:\n"
                  << "  --port PORT         UDP port (configured in server.cpp or client.cpp)\n";
        if (server)
            std::cout << "  --bind ADDRESS      Server listen address (server.cpp)\n";
        else
            std::cout << "  --host HOST         Server address (client.cpp)\n"
                      << "  --headless          Run the client without a window\n";
        std::cout << "  --duration SECONDS  Stop automatically after the given time\n"
                  << "  --help              Show this help\n";
    }

    void shutdown()
    {
        *get() = {};
    }
} // namespace game::options
