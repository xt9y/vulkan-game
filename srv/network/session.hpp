#pragma once

#include "srv/shared/net/endpoint.hpp"

#include <array>
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
    };

    struct state
    {
        std::array<resource, 64> clients{};
    };

    state* get();
    void init();
    void expire(std::chrono::steady_clock::time_point now);
    void shutdown();
} // namespace srv::session
