#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::animation
{
    // Repeated 10 Hz authoritative animation state, not skeleton poses.
    struct resource
    {
        std::uint32_t token = 0;
        std::uint32_t server_tick = 0;
        std::uint32_t instance_id = 0;
        std::uint32_t clip = 0;
        std::uint32_t start_tick = 0;
        std::uint32_t overlay = UINT32_MAX;
        std::uint32_t mask_root = UINT32_MAX;
        float blend = 0;
    };

    struct attachment_event
    {
        std::uint32_t token = 0;
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        std::uint32_t instance_id = 0;
        std::uint32_t joint = 0;
        std::uint32_t kind = 0;
    };

    using event_packet = std::array<std::uint8_t, 28>;
    event_packet encode_event(const attachment_event& event);
    std::optional<attachment_event> decode_event(const std::uint8_t* bytes, std::size_t size);

    using packet = std::array<std::uint8_t, 40>;
    packet encode(const resource& state);
    std::optional<resource> decode(const std::uint8_t* data, std::size_t bytes);
} // namespace net::animation
