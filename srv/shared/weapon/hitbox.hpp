#pragma once
#include "srv/shared/weapon/weapon.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace weapon::hitbox
{
    using namespace vk::util::math;

    struct capsule
    {
        vec3 a{}, b{};
        float radius = .15f;
        std::uint32_t actor = 0;
    };

    inline float sphere(vec3 origin, vec3 direction, vec3 center, float radius)
    {
        const auto oc = origin - center;
        const float b = dot(oc, direction);
        const float delta = b * b - (dot(oc, oc) - radius * radius);
        if (delta < 0) return std::numeric_limits<float>::infinity();
        const float root = std::sqrt(delta);
        if (-b - root >= 0) return -b - root;
        if (-b + root >= 0) return -b + root;
        return std::numeric_limits<float>::infinity();
    }

    inline float intersect(vec3 origin, vec3 direction, const capsule& c)
    {
        const auto axis = c.b - c.a, relative = origin - c.a;
        const float length2 = dot(axis, axis);
        if (length2 < 1e-10f) return sphere(origin, direction, c.a, c.radius);
        const float ar = dot(axis, direction), ao = dot(axis, relative);
        const float A = length2 - ar * ar;
        const float B = length2 * dot(relative, direction) - ar * ao;
        const float C = length2 * dot(relative, relative) - ao * ao -
                        c.radius * c.radius * length2;
        float nearest = std::min(sphere(origin, direction, c.a, c.radius),
                                 sphere(origin, direction, c.b, c.radius));
        if (A > 1e-8f)
        {
            const float delta = B * B - A * C;
            if (delta >= 0)
            {
                const float t = (-B - std::sqrt(delta)) / A;
                const float y = ao + t * ar;
                if (t >= 0 && y >= 0 && y <= length2) nearest = std::min(nearest, t);
            }
        }
        return nearest;
    }

    inline hit trace(vec3 origin, vec3 direction, float range,
                     const std::vector<capsule>& capsules)
    {
        hit result;
        float nearest = range;
        for (const auto& c : capsules)
        {
            if (!c.actor || c.radius <= 0) continue;
            const float t = intersect(origin, direction, c);
            if (t >= nearest) continue;
            const auto axis = c.b - c.a;
            const auto position = origin + direction * t;
            const float u = std::clamp(dot(position - c.a, axis) /
                std::max(dot(axis, axis), 1e-10f), 0.f, 1.f);
            const auto normal = normalize(position - (c.a + axis * u));
            nearest = t;
            result = {true, position, normal, c.actor, 0, true};
        }
        return result;
    }
} // namespace weapon::hitbox
