#include "options.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace game::options
{
    state* get()
    {
        static state data;
        return &data;
    }

    void init(int argc, char** argv)
    {
        state result;
        bool mode_selected = false;
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

            if (option == "--server-only" || option == "--client-only" ||
                option == "--server+client")
            {
                mode selected_mode = mode::server_and_client;

                if (option == "--server-only")
                {
                    selected_mode = mode::server_only;
                }
                else if (option == "--client-only")
                {
                    selected_mode = mode::client_only;
                }

                if (mode_selected && result.mode != selected_mode)
                {
                    throw std::invalid_argument("Conflicting launch modes: choose one mode");
                }

                result.mode = selected_mode;
                mode_selected = true;
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
                    "--host requires --client-only; combined mode connects locally"
                );
            }

            if (result.mode == mode::client_only && bind_specified)
            {
                throw std::invalid_argument("--bind requires a server mode");
            }
        }

        *get() = result;
    }

    void print_help()
    {
        std::cout << "Usage: vulkan-game [MODE] [OPTIONS]\n\n"
                  << "Modes (default: --server+client):\n"
                  << "  --server+client     Start the local server and client\n"
                  << "  --server-only       Start the server without a window\n"
                  << "  --client-only       Connect a client to an existing server\n\n"
                  << "Options:\n"
                  << "  --port PORT         UDP port (default: 27015)\n"
                  << "  --bind ADDRESS      Server listen address (default: 127.0.0.1)\n"
                  << "  --host HOST         Remote server address for --client-only\n"
                  << "  --headless          Run the client without a window\n"
                  << "  --duration SECONDS  Stop automatically after the given time\n"
                  << "  --help              Show this help\n";
    }

    void shutdown()
    {
        *get() = {};
    }
} // namespace game::options
