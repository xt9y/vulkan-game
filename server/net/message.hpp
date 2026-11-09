#pragma once
#include "movement/detail.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::message
{
enum class type : std::uint8_t
{
    hello = 1,
    welcome,
    ping,
    state,
    goodbye,
    scene_request
};

struct resource
{
    message::type type;
    std::uint32_t sequence = 0;
    std::uint32_t token = 0;
    std::uint32_t value = 0;
};

inline constexpr std::size_t packet_size = 20;

using packet = std::array<std::uint8_t, packet_size>;
packet encode(const resource &message)
{
    packet p{'V', 'G', 'A', 'M', 1, std::uint8_t(message.type), 0, 0};
    net::movement::detail::write_fields(p.data() + 8, message.sequence, message.token, message.value);

    return p;
}

std::optional<resource> decode(const std::uint8_t *p, std::size_t size)
{
    if (!p || size != packet_size || p[0] != 'V' || p[1] != 'G' || p[2] != 'A' || p[3] != 'M' || p[4] != 1 || p[5] < 1 || p[5] > 6 || p[6] || p[7])
        return std::nullopt;
    resource v{};
    v.type = type(p[5]);
    net::movement::detail::read_fields(p + 8, v.sequence, v.token, v.value);

    return v;
}

bool newer(std::uint32_t sequence, std::uint32_t previous)
{
    const auto difference = sequence - previous;

    return difference != 0 && difference < 0x80000000u;
}

} // namespace net::message
