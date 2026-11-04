#pragma once

#include "srv/shared/weapon/weapon.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::weapon
{
    using action = ::weapon::action;
    using result = ::weapon::result;
    using reload_phase = ::weapon::reload_phase;

    struct command
    {
        std::uint32_t token = 0, sequence = 0;
        action kind = action::fire;
        float yaw = 0, pitch = 0;
        std::uint32_t weapon_id = 0;
    };

    struct snapshot
    {
        std::uint32_t token = 0, sequence = 0, tick = 0;
        std::uint32_t magazine = 0, reserve = 0;
        reload_phase reload = reload_phase::none;
        result outcome = result::none;
        float reload_elapsed = 0, reload_duration = 0, refill_time = 0;
        std::uint32_t weapon_id = 0;
        std::uint32_t active_weapon_id = 0;
    };

    struct shot
    {
        snapshot state;
        ::weapon::hit impact;
    };

    using command_packet = std::array<std::uint8_t, 32>;
    using snapshot_packet = std::array<std::uint8_t, 56>;
    using shot_packet = std::array<std::uint8_t, 96>;
    command_packet encode_command(const command& value);
    std::optional<command> decode_command(const std::uint8_t* bytes, std::size_t size);
    snapshot_packet encode_snapshot(const snapshot& value);
    std::optional<snapshot> decode_snapshot(const std::uint8_t* bytes, std::size_t size);
    shot_packet encode_shot(const shot& value);
    std::optional<shot> decode_shot(const std::uint8_t* bytes, std::size_t size);
} // namespace net::weapon
