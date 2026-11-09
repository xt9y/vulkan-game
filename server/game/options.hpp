#pragma once
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace game::options
{
enum class mode
{
    server_only,
    client_only
};

struct client_graphics
{
    int width, height;
    std::string title, vertex_shader, fragment_shader;
    std::string culling_shader, lighting_shader, tonemap_shader;
    std::string fullscreen_shader, presentation_shader;

    float exposure;
};

struct state
{
    options::mode mode;
    std::string host, bind_host;
    std::uint16_t port;

    double duration;

    bool headless, help;

    client_graphics graphics;
};

state *get();
void init(int argc, char **argv, mode selected_mode, state configured);
void print_help();
void shutdown();

state *get()
{
    static state value;

    return &value;
}

void init(int argc, char **argv, mode selected_mode, state initial)
{
    initial.mode = selected_mode;

    bool host_given = false, bind_given = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--help") {
            initial.help = true;

            continue;
        }

        if (arg == "--headless") {
            initial.headless = true;

            continue;
        }

        if (arg != "--port" && arg != "--duration" && arg != "--host" && arg != "--bind")
            throw std::invalid_argument("Unknown option: " + arg);

        if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
        const std::string value = argv[i];
        std::size_t end = 0;

        if (arg == "--port") {
            const auto port = std::stoul(value, &end);

            if (end != value.size() || value.empty() || value[0] == '-' || !port || port > 65535)
                throw std::invalid_argument("Port must be in 1..65535");
            initial.port = std::uint16_t(port);
        }
        else if (arg == "--duration") {
            initial.duration = std::stod(value, &end);

            if (end != value.size() || !std::isfinite(initial.duration) || initial.duration <= 0)
                throw std::invalid_argument("Duration must be a positive number of seconds");
        }
        else if (arg == "--host") {
            initial.host = value;
            host_given = true;
        }
        else {
            initial.bind_host = value;
            bind_given = true;
        }
    }

    if (!initial.help) {
        if (initial.mode == mode::server_only && initial.headless)
            throw std::invalid_argument("--headless requires a client mode");

        if (initial.mode != mode::client_only && host_given)
            throw std::invalid_argument("--host is only supported by vulkan-client");

        if (initial.mode == mode::client_only && bind_given)
            throw std::invalid_argument("--bind is only supported by vulkan-server");
    }

    if (!initial.port || (initial.mode == mode::server_only && initial.bind_host.empty()))
        throw std::invalid_argument("A server bind address and UDP port are required");

    if (initial.mode == mode::client_only) {
        const auto &g = initial.graphics;

        if (initial.host.empty() || g.width <= 0 || g.height <= 0 || g.title.empty() ||
            g.vertex_shader.empty() || g.fragment_shader.empty() ||
            g.culling_shader.empty() || g.lighting_shader.empty() || g.tonemap_shader.empty() || g.fullscreen_shader.empty() ||
            g.presentation_shader.empty() || g.exposure < 0)
            throw std::invalid_argument("Client graphics configuration is incomplete");
    }
    *get() = std::move(initial);
}

void print_help()
{
    const bool server = get()->mode == mode::server_only;
    std::cout << (server ? "Usage: vulkan-server [OPTIONS]\n\n" : "Usage: vulkan-client [OPTIONS]\n\n");
    std::cout << "Options:\n  --port PORT         UDP port (configured in server.cpp or client.cpp)\n";

    if (server)
        std::cout << "  --bind ADDRESS      Server listen address (server.cpp)\n";
    else
        std::cout << "  --host HOST         Server address (client.cpp)\n"
                  << "  --headless          Run the client without a window\n";
    std::cout << "  --duration SECONDS  Stop automatically after the given time\n"
              << "  --help              Show this help\n";
}

void shutdown() { *get() = {}; }

} // namespace game::options
