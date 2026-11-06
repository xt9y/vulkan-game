#pragma once

#include <network/endpoint.hpp>
#include "srv/shared/net/weapon.hpp"
#include "srv/shared/player/movement.hpp"

#include <chrono>
#include <deque>
#include <vector>

namespace srv::session
{
    struct resource
    {
        net::endpoint::resource endpoint;
        std::uint32_t token = 0;
        std::uint32_t last_sequence = 0;
        bool has_sequence = false;
        bool active = false;
        std::chrono::steady_clock::time_point last_seen;
        ::player::state avatar;

        struct owned_weapon
        {
            std::uint32_t id = 0;
            ::weapon::state state;
            std::optional<net::weapon::snapshot> published;
        };

        std::vector<owned_weapon> weapons;
        std::uint32_t active_weapon = 0;
        std::uint32_t weapon_sequence = 0;
        std::deque<net::weapon::command> weapon_commands;

        struct pending_shot
        {
            net::weapon::shot value;
            std::uint32_t repeats = 3;
        };

        std::deque<pending_shot> shots;
        std::optional<net::weapon::shot> last_shot;
        std::deque<::player::input> inputs;
        std::uint32_t processed_input = 0;
        std::uint32_t scene_request = 0;
        bool scene_pending = false;
    };

    struct state
    {
        std::vector<resource> clients;
    };

    state* get();
    void init();
    void expire(std::chrono::steady_clock::time_point now);
    void shutdown();
} // namespace srv::session
