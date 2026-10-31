#pragma once

#include <cstdint>
#include <string>

namespace game::options
{
    enum class mode
    {
        server_and_client,
        server_only,
        client_only
    };

    struct state
    {
        options::mode mode = options::mode::server_and_client;

        std::string host = "127.0.0.1";
        std::string bind_host = "127.0.0.1";
        std::uint16_t port = 27015;

        double duration = 0;
        bool headless = false;
        bool help = false;
    };

    state* get();
    void init(int argc, char** argv);
    void print_help();
    void shutdown();
} // namespace game::options
