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

        if (!result.port || (result.mode == mode::server_only && result.bind_host.empty()))
            throw std::invalid_argument("A server bind address and UDP port are required");

        if (result.mode == mode::client_only)
        {
            const auto& g = result.graphics;
            if (result.host.empty() || g.width <= 0 || g.height <= 0 ||
                g.title.empty() || g.vertex_shader.empty() || g.fragment_shader.empty() ||
                g.gbuffer == gbuffer_precision::unspecified ||
                g.surface == surface_format::unspecified ||
                g.depth == depth_precision::unspecified ||
                g.present == presentation::unspecified ||
                g.culling_shader.empty() || g.lighting_shader.empty() ||
                g.tonemap_shader.empty() || g.fullscreen_shader.empty() ||
                g.presentation_shader.empty() || g.exposure < 0 || g.sun_intensity < 0 ||
                (g.sun_direction.x == 0 && g.sun_direction.y == 0 && g.sun_direction.z == 0))
                throw std::invalid_argument("Client graphics configuration is incomplete");
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
