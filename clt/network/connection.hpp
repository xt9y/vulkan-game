#pragma once

#include "srv/shared/net/socket.hpp"
#include "srv/shared/scene/scene.hpp"

#include "srv/shared/net/movement.hpp"

#include <deque>
#include <optional>

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
        std::atomic<std::uint64_t> player_snapshots{0};
        std::atomic<bool> scene_complete{false};

        std::mutex scene_mutex;
        std::shared_ptr<const ::scene::state> scene;

        std::mutex player_mutex;

        std::deque<::player::input> outgoing;
        std::optional<net::movement::snapshot> authority;

        std::thread worker;
        bool initialized = false;
    };

    state* get();
    std::shared_ptr<const ::scene::state> snapshot();
    void send_input(const ::player::input& input);
    std::optional<net::movement::snapshot> take_authority();
    void init();
    void shutdown() noexcept;
} // namespace clt::connection
