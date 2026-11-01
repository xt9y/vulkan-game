#pragma once

#include "srv/shared/net/socket.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace clt::connection
{
    struct state
    {
        // Set configuration before init. The worker owns the socket until shutdown joins it.
        std::string host = "127.0.0.1";
        std::uint16_t port = 27015;
        net::socket::resource socket;

        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> server_tick{0};

        std::thread worker;
        bool initialized = false;
    };

    state* get();
    void init();
    void shutdown() noexcept;
} // namespace clt::connection
