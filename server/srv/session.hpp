#pragma once
#include "../net/limits.hpp"
#include "../player.hpp"
#include "../net/weapon.hpp"
#include <network/endpoint.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace srv::session {
struct resource {
        net::endpoint::resource endpoint;
        std::uint32_t token = 0;
        std::uint32_t last_sequence = 0;
        bool has_sequence = false;
        bool active = false;
        std::chrono::steady_clock::time_point last_seen;
        ::player::state avatar;
        ::weapon::state weapon;
        std::uint32_t weapon_sequence = 0;
        std::deque<net::weapon::command> weapon_commands;
        std::uint32_t editor_light_sequence=0;
        std::uint32_t editor_light_index=UINT32_MAX;
        struct pending_shot {
            net::weapon::shot value;
            std::uint32_t repeats = 3;
        };
        std::deque<pending_shot> shots;
        std::optional<net::weapon::shot> last_shot;
        std::deque<::player::input> inputs;
        std::uint32_t processed_input = 0;
        std::uint32_t scene_request = 0;
        bool scene_pending = false;
        std::uint32_t queued_scene_batch=UINT32_MAX;
        std::chrono::steady_clock::time_point queued_scene_at{};
    };
    struct state {
        std::vector<resource> clients;
    };
    state* get();
    void init();
    void expire(std::chrono::steady_clock::time_point now);
    void shutdown();
state* get() {
        static state data;
        return &data;
    }
    void init() {
        *get() = {};
        get()->clients.resize(net::limits::max_clients);
    }
    void expire(std::chrono::steady_clock::time_point now) {
        for (auto& client : get()->clients) {
            if (client.active && now - client.last_seen > std::chrono::seconds(net::limits::timeout_seconds)) 
                client.active = false;
        }
    }
    void shutdown() {
        *get() = {};
    }

}
