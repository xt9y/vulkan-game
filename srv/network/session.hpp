#pragma once

#include "srv/shared/net/endpoint.hpp"

#include "srv/shared/player/movement.hpp"

#include <deque>

#include <vector>
#include <chrono>

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
