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

        if (result.host.empty() || result.bind_host.empty() ||
            !result.port || !result.simulation.tick_hz || !result.simulation.max_clients ||
            !result.simulation.receive_packets || !result.simulation.fragments_per_tick ||
            !result.simulation.inputs_per_tick || !result.simulation.animation_period ||
            !result.simulation.animations_per_tick || !result.simulation.timeout_seconds ||
            !result.simulation.receive_ms || !result.simulation.fragments_ms ||
            !result.simulation.generation_commit_ticks ||
            !result.simulation.terrain_scale || !result.simulation.detail_scale ||
            result.player.walk_speed <= 0 || result.player.sprint_speed <= 0 ||
            result.player.height <= 0 ||
            result.player.radius <= 0 || result.player.gravity <= 0 ||
            result.view.fov <= 0 || result.view.near_plane <= 0 ||
            result.view.far_plane <= result.view.near_plane ||
            result.graphics.width <= 0 || result.graphics.height <= 0 ||
            result.graphics.vertex_shader.empty() || result.graphics.fragment_shader.empty() ||
            result.graphics.scene_request_ms == 0 || result.graphics.movement_ms == 0 ||
            result.graphics.retry_ms == 0 ||
            result.graphics.retry_max_ms < result.graphics.retry_ms ||
            result.graphics.timeout_ms == 0 ||
            result.graphics.heartbeat_ms == 0 ||
            result.graphics.correction_decay <= 0 || result.graphics.correction_decay >= 1 ||
            result.graphics.exposure < 0 ||
            result.graphics.max_exposure <= result.graphics.min_exposure ||
            result.graphics.max_fov <= result.graphics.min_fov ||
            result.graphics.max_sensitivity <= result.graphics.min_sensitivity ||
            result.graphics.exposure_step <= 0 || result.graphics.fov_step <= 0 ||
            result.graphics.sensitivity_step <= 0 ||
            result.view.fov < result.graphics.min_fov ||
            result.view.fov > result.graphics.max_fov ||
            result.view.sensitivity < result.graphics.min_sensitivity ||
            result.view.sensitivity > result.graphics.max_sensitivity ||
            result.graphics.exposure > result.graphics.max_exposure ||
            result.graphics.correction_limit <= 0)
            throw std::invalid_argument("Game configuration is incomplete or invalid");
        *get() = std::move(result);
    }

    void print_help()
    {
        const bool server = get()->mode == mode::server_only;
        std::cout << (server ? "Usage: vulkan-server [OPTIONS]\n\n"
                            : "Usage: vulkan-client [OPTIONS]\n\n")
                  << "Options:\n"
                  << "  --port PORT         UDP port (configured in game::config)\n";
        if (server)
            std::cout << "  --bind ADDRESS      Server listen address (game::config)\n";
        else
            std::cout << "  --host HOST         Server address (game::config)\n"
                      << "  --headless          Run the client without a window\n";
        std::cout << "  --duration SECONDS  Stop automatically after the given time\n"
                  << "  --help              Show this help\n";
    }

    void shutdown()
    {
        *get() = {};
    }
} // namespace game::options
