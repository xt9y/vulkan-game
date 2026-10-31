#pragma once

#include "srv/shared/net/socket.hpp"
#include "srv/shared/scene/scene.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace clt::connection
{
    struct state
    {
        std::string host = "127.0.0.1";
        std::uint16_t port = 27015;
        net::socket::resource socket;

        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> server_tick{0};

        std::mutex scene_mutex;
        std::shared_ptr<const ::scene::state> scene;

        std::thread worker;
        bool initialized = false;
    };

    state* get();
    std::shared_ptr<const ::scene::state> snapshot();
    void init();
    void shutdown() noexcept;
} // namespace clt::connection
