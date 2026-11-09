#pragma once
#include "../weapon.hpp"
#include "movement/detail.hpp"
#include <array>
#include <cmath>
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
using shot_packet = std::array<std::uint8_t, 92>;

namespace
{
using namespace net::movement::detail;
bool header(const std::uint8_t *p, std::size_t n, std::size_t expected, std::uint8_t type)
{
    return p && n == expected && p[0] == 'V' && p[1] == 'G' && p[2] == 'U' && p[3] == 'N' && p[4] == 2 && p[5] == type && !p[6] && !p[7];
}

void write_state(std::uint8_t *p, const snapshot &v)
{
    write_fields(p + 8, v.token, v.sequence, v.tick, v.magazine, v.reserve, std::uint32_t(v.reload), std::uint32_t(v.outcome), v.reload_elapsed, v.reload_duration, v.refill_time, v.weapon_id, v.active_weapon_id);
}

std::optional<snapshot> read_state(const std::uint8_t *p)
{
    snapshot v{};
    std::uint32_t reload = 0, outcome = 0;
    read_fields(p + 8, v.token, v.sequence, v.tick, v.magazine, v.reserve, reload, outcome, v.reload_elapsed, v.reload_duration, v.refill_time, v.weapon_id, v.active_weapon_id);
    v.reload = reload_phase(reload);
    v.outcome = result(outcome);

    if (!v.token || !v.weapon_id || !v.active_weapon_id || reload > 2 || outcome > std::uint32_t(result::equipped) || !std::isfinite(v.reload_elapsed) || !std::isfinite(v.reload_duration) ||
        !std::isfinite(v.refill_time) || v.reload_elapsed < 0 || v.reload_duration < 0 || v.refill_time < 0 || v.reload_elapsed > v.reload_duration || v.refill_time > v.reload_duration)
        return std::nullopt;

    return v;
}
} // namespace
command_packet encode_command(const command &v)
{
    command_packet p{'V', 'G', 'U', 'N', 2, 1, 0, 0};
    net::movement::detail::write_fields(p.data() + 8, v.token, v.sequence, std::uint32_t(v.kind), v.yaw, v.pitch, v.weapon_id);

    return p;
}

std::optional<command> decode_command(const std::uint8_t *p, std::size_t n)
{
    if (!header(p, n, command_packet{}.size(), 1)) return std::nullopt;
    command v{};
    std::uint32_t kind = 0;
    net::movement::detail::read_fields(p + 8, v.token, v.sequence, kind, v.yaw, v.pitch, v.weapon_id);
    v.kind = action(kind);

    if (!v.token || !v.sequence || !v.weapon_id || kind < 1 || kind > 3 || !std::isfinite(v.yaw) || !std::isfinite(v.pitch) || std::abs(v.yaw) > 1000 || std::abs(v.pitch) > 1.570797f)
        return std::nullopt;

    return v;
}

snapshot_packet encode_snapshot(const snapshot &v)
{
    snapshot_packet p{'V', 'G', 'U', 'N', 2, 2, 0, 0};
    write_state(p.data(), v);

    return p;
}

std::optional<snapshot> decode_snapshot(const std::uint8_t *p, std::size_t n)
{
    if (!header(p, n, snapshot_packet{}.size(), 2)) return std::nullopt;

    return read_state(p);
}

shot_packet encode_shot(const shot &v)
{
    shot_packet p{'V', 'G', 'U', 'N', 2, 3, 0, 0};
    write_state(p.data(), v.state);
    const auto &h = v.impact;
    net::movement::detail::write_fields(p.data() + 56, std::uint32_t(h.impacted), h.position.x, h.position.y, h.position.z, h.normal.x, h.normal.y, h.normal.z, h.instance, h.triangle);

    return p;
}

std::optional<shot> decode_shot(const std::uint8_t *p, std::size_t n)
{
    if (!header(p, n, shot_packet{}.size(), 3)) return std::nullopt;

    const auto state = read_state(p);
    if (!state) return std::nullopt;
    shot v{};
    v.state = *state;
    std::uint32_t impacted = 0;
    auto &h = v.impact;
    net::movement::detail::read_fields(p + 56, impacted, h.position.x, h.position.y, h.position.z, h.normal.x, h.normal.y, h.normal.z, h.instance, h.triangle);

    if (impacted > 1) return std::nullopt;
    h.impacted = impacted == 1;

    const auto finite = [](float x)
    { return std::isfinite(x) && std::abs(x) < 1000000.f; };

    if (!finite(h.position.x) || !finite(h.position.y) || !finite(h.position.z) || !finite(h.normal.x) || !finite(h.normal.y) || !finite(h.normal.z))
        return std::nullopt;

    if (h.impacted && (state->outcome != result::fired || !h.instance || std::abs(vk::util::math::length_squared(h.normal) - 1.f) > .01f))
        return std::nullopt;

    return v;
}

} // namespace net::weapon
