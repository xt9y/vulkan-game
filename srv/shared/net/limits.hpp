#pragma once
#include <cstddef>
#include <cstdint>

namespace net::limits
{
    inline constexpr std::uint32_t tick_hz = 60;
    inline constexpr std::uint32_t max_clients = 64;
    inline constexpr std::uint32_t timeout_seconds = 5;
    inline constexpr std::uint32_t receive_packets = 256;
    inline constexpr std::uint32_t receive_ms = 2;
    inline constexpr std::uint32_t inputs_per_tick = 4;
    inline constexpr std::uint32_t animation_period = 6;
    inline constexpr std::uint32_t animations_per_tick = 64;
    inline constexpr std::uint32_t generation_slack_ms = 2;
    inline constexpr std::uint32_t generation_commit_ticks = 8;
    inline constexpr std::uint32_t connection_timeout_ms = 3000;
    inline constexpr std::uint32_t retry_ms = 250;
    inline constexpr std::uint32_t retry_max_ms = 1000;
    inline constexpr std::uint32_t heartbeat_ms = 50;
    inline constexpr std::uint32_t scene_request_ms = 100;
    inline constexpr std::uint32_t movement_ms = 16;
}
