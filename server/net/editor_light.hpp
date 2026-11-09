#pragma once
#include "../scene.hpp"
#include "movement/detail.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace net::editor_light
{

enum class kind : std::uint8_t
{
    create = 1,
    edit = 2,
    state = 3,
    ack = 4,
    remove = 5
};

struct resource
{
    kind type = kind::state;

    std::uint32_t token = 0, sequence = 0, index = 0, revision = 0;
    ::scene::point_light light{};
};

using packet = std::array<std::uint8_t, 56>;

inline constexpr std::array<std::string_view, 8> property_keys{
    "pos_x", "pos_y", "pos_z", "color_r", "color_g", "color_b", "radius", "strength"
};

template <class Light>
auto property_fields(Light &light)
{
    return std::array{&light.position.x, &light.position.y, &light.position.z, &light.color.x, &light.color.y, &light.color.z, &light.radius, &light.strength};
}

inline bool valid(const ::scene::point_light &l)
{
    const auto in = [](float value, float maximum)
    {
        return std::isfinite(value) && value >= 0 && value <= maximum;
    };

    const auto coord = [](float value)
    {
        return std::isfinite(value) && std::abs(value) <= 500.f;
    };

    return coord(l.position.x) && coord(l.position.y) && coord(l.position.z) && in(l.color.x, 1) && in(l.color.y, 1) && in(l.color.z, 1) && in(l.radius, 64) && l.radius >= .05f && in(l.strength, 20);
}

inline packet encode(const resource &v)
{
    packet p{'V', 'L', 'I', 'T', 1, std::uint8_t(v.type), std::uint8_t(v.light.enabled ? 1 : 0), 0};
    const auto &l = v.light;
    net::movement::detail::write_fields(p.data() + 8, v.token, v.sequence, v.index, v.revision, l.position.x, l.position.y, l.position.z, l.radius, l.color.x, l.color.y, l.color.z, l.strength);

    return p;
}

inline std::optional<resource> decode(const std::uint8_t *p, std::size_t n)
{
    if (!p || n != packet{}.size() || p[0] != 'V' || p[1] != 'L' || p[2] != 'I' || p[3] != 'T' || p[4] != 1 || p[5] < 1 || p[5] > 5 || p[6] > 1 || p[7] != 0) return std::nullopt;
    resource v{};
    v.type = kind(p[5]);
    auto &l = v.light;
    net::movement::detail::read_fields(p + 8, v.token, v.sequence, v.index, v.revision, l.position.x, l.position.y, l.position.z, l.radius, l.color.x, l.color.y, l.color.z, l.strength);
    l.enabled = p[6] != 0;

    if (!v.token || !v.sequence || !valid(l) || (v.type == kind::create ? v.index != UINT32_MAX : v.index >= 64))
        return std::nullopt;

    return v;
}

} // namespace net::editor_light
