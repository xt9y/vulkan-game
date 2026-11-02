#pragma once

#include "srv/shared/net/socket.hpp"
#include "srv/shared/scene/scene.hpp"

#include "srv/shared/net/movement.hpp"
#include "srv/shared/net/animation.hpp"

#include <chrono>
#include <deque>
#include <vector>
#include <exception>
#include <optional>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace clt::connection
{
    struct state
    {
        std::string host;
        std::uint16_t port = 0;
        net::socket::resource socket;

        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> server_tick{0};
        std::atomic<std::uint64_t> player_snapshots{0};
        std::atomic<bool> scene_complete{false};

        std::mutex animation_mutex;
        std::unordered_map<std::uint32_t, net::animation::resource> animations;
        std::deque<net::animation::attachment_event> attachment_events;
        std::uint32_t last_attachment_sequence = 0;

        std::mutex scene_mutex;
        std::shared_ptr<const ::scene::state> scene;
        std::vector<::scene::state> pending_scene;
        std::chrono::steady_clock::time_point last_publish{};

        std::mutex error_mutex;
        std::exception_ptr worker_error;

        std::mutex player_mutex;

        std::deque<::player::input> outgoing;
        std::optional<net::movement::snapshot> authority;
        std::optional<::player::parameters_t> server_parameters;

        std::thread worker;
        bool initialized = false;
    };

    state* get();
    std::shared_ptr<const ::scene::state> snapshot();
    std::optional<net::animation::resource> animation(std::uint32_t instance_id);
    std::vector<net::animation::attachment_event> take_attachment_events();
    void check_error();
    void send_input(const ::player::input& input);
    std::optional<net::movement::snapshot> take_authority();
    std::optional<::player::parameters_t> movement_parameters();
    void init();
    void shutdown() noexcept;
} // namespace clt::connection
