#pragma once
#include "srv/shared/net/movement.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::blood
{
    using vk::util::math::vec3;
    struct event
    {
        std::uint32_t token = 0, id = 0, target = 0, seed = 0;
        vec3 position{}, normal{}, direction{};
        float intensity = 1;
        std::uint32_t tick = 0;
    };
    using packet = std::array<std::uint8_t, 68>;

    inline packet encode(const event& v)
    {
        using namespace net::movement::detail;
        packet p{'V', 'B', 'L', 'D', 1, 1, 0, 0};
        put(p.data() + 8, v.token);
        put(p.data() + 12, v.id);
        put(p.data() + 16, v.target);
        put(p.data() + 20, v.seed);
        const float coordinates[9] = {
            v.position.x, v.position.y, v.position.z,
            v.normal.x, v.normal.y, v.normal.z,
            v.direction.x, v.direction.y, v.direction.z
        };
        for (int i = 0; i < 9; ++i) put_float(p.data() + 24 + i * 4, coordinates[i]);
        put_float(p.data() + 60, v.intensity);
        put(p.data() + 64, v.tick);
        return p;
    }

    inline std::optional<event> decode(const std::uint8_t* p, std::size_t size)
    {
        using namespace net::movement::detail;
        if (!p || size != packet{}.size() || p[0] != 'V' || p[1] != 'B' ||
            p[2] != 'L' || p[3] != 'D' || p[4] != 1 || p[5] != 1 ||
            p[6] || p[7]) return std::nullopt;
        event e;
        e.token = get(p + 8);
        e.id = get(p + 12);
        e.target = get(p + 16);
        e.seed = get(p + 20);
        float data[9];
        for (int i = 0; i < 9; ++i)
        {
            data[i] = get_float(p + 24 + i * 4);
            if (!std::isfinite(data[i]) || std::abs(data[i]) > 100000.f)
                return std::nullopt;
        }
        e.position = {data[0], data[1], data[2]};
        e.normal = {data[3], data[4], data[5]};
        e.direction = {data[6], data[7], data[8]};
        e.intensity = get_float(p + 60);
        e.tick = get(p + 64);
        const float n2 = data[3] * data[3] + data[4] * data[4] + data[5] * data[5];
        const float d2 = data[6] * data[6] + data[7] * data[7] + data[8] * data[8];
        if (!e.token || !e.id || !e.target || !std::isfinite(e.intensity) ||
            e.intensity <= 0 || e.intensity > 10 ||
            n2 < .95f || n2 > 1.05f || d2 < 1e-8f) return std::nullopt;
        return e;
    }
} // namespace net::blood
