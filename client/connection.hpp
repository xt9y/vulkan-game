#pragma once
#include "../server/animation.hpp"
#include "../server/editor_light.hpp"
#include "../server/movement.hpp"
#include "../server/net_weapon.hpp"
#include "../server/player.hpp"
#include "../server/scene.hpp"
#include <network/secure.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace clt::connection {
    struct state {
        std::string host;
        std::uint16_t port = 0;
        net::secure::resource socket;
        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> server_tick{0};
        std::atomic<bool> scene_complete{false};
        std::mutex animation_mutex;
        std::unordered_map<std::uint32_t, net::animation::resource> animations;
        std::mutex scene_mutex;
        std::shared_ptr<const ::scene::state> scene;
        std::vector<::scene::state> pending_scene;
        std::chrono::steady_clock::time_point last_publish{};
        std::array<std::uint32_t,64> light_versions{};
        std::array<std::optional<net::editor_light::resource>,64> light_pending{};
        std::mutex error_mutex;
        std::exception_ptr worker_error;
        std::mutex player_mutex;
        std::deque<::player::input> outgoing;
        std::optional<net::movement::snapshot> authority;
        std::optional<::player::parameters_t> server_parameters;
        std::mutex editor_mutex;
        std::deque<net::editor_light::resource> editor_outgoing;
        std::uint32_t editor_sequence=0;
        std::mutex weapon_mutex;
        std::deque<net::weapon::command> weapon_outgoing;
        std::deque<net::weapon::snapshot> weapon_updates;
        std::deque<net::weapon::shot> weapon_shots;
        std::uint32_t weapon_ack = 0;
        std::uint32_t weapon_seen_id=0,weapon_seen_sequence=0,weapon_seen_tick=0;
        std::thread worker;
        bool initialized = false;
    };
    state* get();
    std::shared_ptr<const ::scene::state> snapshot();
    std::optional<net::animation::resource> animation(std::uint32_t instance_id);
    bool send_editor_light(net::editor_light::resource command);
    bool send_weapon(net::weapon::command command);
    std::deque<net::weapon::snapshot> take_weapon_updates();
    std::deque<net::weapon::shot> take_weapon_shots();
    void check_error();
    void send_input(const ::player::input& input);
    std::optional<net::movement::snapshot> take_authority();
    std::optional<::player::parameters_t> movement_parameters();
    void init();
    void shutdown() noexcept;

} // namespace clt::connection
