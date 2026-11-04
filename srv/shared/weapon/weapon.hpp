#pragma once

#include "srv/shared/net/message.hpp"
#include "srv/shared/scene/scene.hpp"
#include "srv/shared/spatial/index.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace weapon
{
    using vk::util::math::vec3;

    enum class action : std::uint32_t
    {
        fire = 1,
        reload = 2,
        equip = 3
    };
    enum class reload_phase : std::uint32_t
    {
        none,
        tactical,
        empty
    };
    enum class result : std::uint32_t
    {
        none,
        fired,
        cooldown,
        empty,
        reloading,
        reload_started,
        reload_unnecessary,
        equipped
    };

    struct timing
    {
        float reload_duration = 0;
        float empty_reload_duration = 0;
        float refill_time = 0;
        float empty_refill_time = 0;
    };

    struct definition
    {
        std::uint32_t id = 0;
        std::uint32_t magazine_capacity = 0;
        std::uint32_t initial_reserve = 0;
        double fire_interval = 0;
        float range = 0;
        timing reload;
    };

    inline void validate(const definition& value)
    {
        const auto& t = value.reload;
        if (!value.id || !value.magazine_capacity || !std::isfinite(value.fire_interval) ||
            value.fire_interval <= 0 || !std::isfinite(value.range) || value.range <= 0 ||
            !std::isfinite(t.reload_duration) || !std::isfinite(t.empty_reload_duration) ||
            !std::isfinite(t.refill_time) || !std::isfinite(t.empty_refill_time) ||
            t.reload_duration <= 0 || t.empty_reload_duration <= 0 || t.refill_time < 0 ||
            t.refill_time > t.reload_duration || t.empty_refill_time < 0 ||
            t.empty_refill_time > t.empty_reload_duration)
        {
            throw std::invalid_argument("Invalid weapon definition");
        }
    }

    class registry
    {
        std::vector<definition> entries;

    public:
        void add(definition value)
        {
            validate(value);
            if (find(value.id))
            {
                throw std::invalid_argument("Duplicate weapon ID");
            }
            entries.push_back(std::move(value));
        }

        const definition* find(std::uint32_t id) const
        {
            const auto found = std::find_if(
                entries.begin(),
                entries.end(),
                [id](const definition& value)
                {
                    return value.id == id;
                }
            );
            return found == entries.end() ? nullptr : &*found;
        }

        const std::vector<definition>& all() const
        {
            return entries;
        }

        bool empty() const
        {
            return entries.empty();
        }
    };

    struct state
    {
        std::uint32_t magazine = 0;
        std::uint32_t reserve = 0;
        std::uint32_t sequence = 0;
        reload_phase reload = reload_phase::none;
        result last_result = result::none;
        double next_fire = 0;
        double reload_started = 0;
        double reload_finishes = 0;
        double refill_at = 0;
        bool refilled = false;
    };

    inline state make_state(const definition& settings)
    {
        state value;
        value.magazine = settings.magazine_capacity;
        value.reserve = settings.initial_reserve;
        return value;
    }

    inline void advance(state& value, double now, const definition& settings)
    {
        if (value.reload == reload_phase::none)
        {
            return;
        }
        if (!value.refilled && now >= value.refill_at)
        {
            const auto amount = std::min(
                settings.magazine_capacity - std::min(value.magazine, settings.magazine_capacity),
                value.reserve
            );
            value.magazine += amount;
            value.reserve -= amount;
            value.refilled = true;
        }
        if (now >= value.reload_finishes)
        {
            value.reload = reload_phase::none;
        }
    }

    // One edge-triggered command is one semiautomatic attempt. Time comes only from the server.
    inline result apply(
        state& value, std::uint32_t sequence, action command, double now, const definition& settings
    )
    {
        advance(value, now, settings);
        if (!sequence || !net::message::newer(sequence, value.sequence))
        {
            return result::none;
        }
        value.sequence = sequence;
        if (command == action::equip)
        {
            return value.last_result = result::equipped;
        }
        if (command != action::fire && command != action::reload)
        {
            return value.last_result = result::none;
        }
        if (value.reload != reload_phase::none)
        {
            return value.last_result = result::reloading;
        }
        if (command == action::reload)
        {
            if (value.magazine == settings.magazine_capacity || !value.reserve)
            {
                return value.last_result = result::reload_unnecessary;
            }
            const auto& reload = settings.reload;
            const bool empty = !value.magazine;
            value.reload = empty ? reload_phase::empty : reload_phase::tactical;
            value.reload_started = now;
            value.reload_finishes =
                now + (empty ? reload.empty_reload_duration : reload.reload_duration);
            value.refill_at = now + (empty ? reload.empty_refill_time : reload.refill_time);
            value.refilled = false;
            return value.last_result = result::reload_started;
        }
        if (now < value.next_fire)
        {
            return value.last_result = result::cooldown;
        }
        if (!value.magazine)
        {
            return value.last_result = result::empty;
        }
        --value.magazine;
        value.next_fire = now + settings.fire_interval;
        return value.last_result = result::fired;
    }

    inline vec3 direction(float yaw, float pitch)
    {
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }

    struct triangle
    {
        vec3 a, b, c;
        std::uint32_t instance = 0, index = 0;
    };

    struct hit
    {
        bool impacted = false;
        vec3 position{}, normal{};
        std::uint32_t instance = 0, triangle = 0;
        bool flesh = false;
    };

    struct world
    {
        std::vector<triangle> triangles;
        spatial::index index;

        void append(
            const std::vector<scene::mesh>& meshes, const std::vector<scene::instance>& instances
        )
        {
            using namespace vk::util::math;
            std::vector<spatial::bounds> boxes;
            for (const auto& instance : instances)
            {
                const auto mesh = std::find_if(
                    meshes.begin(),
                    meshes.end(),
                    [&](const scene::mesh& m)
                    {
                        return m.id == instance.mesh_id;
                    }
                );
                // First-person weapon skins are presentation, never world collision.
                if (mesh == meshes.end() || mesh->skin_id)
                {
                    continue;
                }
                auto transform = [&](vec3 p)
                {
                    return instance.pose.position + rotate(
                                                        instance.pose.rotation,
                                                        {p.x * instance.pose.scale.x,
                                                         p.y * instance.pose.scale.y,
                                                         p.z * instance.pose.scale.z}
                                                    );
                };
                for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
                {
                    const auto a = mesh->indices[i], b = mesh->indices[i + 1],
                               c = mesh->indices[i + 2];
                    if (a >= mesh->vertices.size() || b >= mesh->vertices.size() ||
                        c >= mesh->vertices.size())
                    {
                        continue;
                    }
                    triangle t{
                        transform(mesh->vertices[a].position),
                        transform(mesh->vertices[b].position),
                        transform(mesh->vertices[c].position),
                        instance.id,
                        std::uint32_t(i / 3)
                    };
                    triangles.push_back(t);
                    boxes.push_back(
                        {{std::min({t.a.x, t.b.x, t.c.x}),
                          std::min({t.a.y, t.b.y, t.c.y}),
                          std::min({t.a.z, t.b.z, t.c.z})},
                         {std::max({t.a.x, t.b.x, t.c.x}),
                          std::max({t.a.y, t.b.y, t.c.y}),
                          std::max({t.a.z, t.b.z, t.c.z})}}
                    );
                }
            }
            index.append(boxes);
        }

        hit trace(vec3 origin, vec3 direction, float range) const
        {
            using namespace vk::util::math;
            hit nearest;
            float distance = range;
            const auto end = origin + direction * range;
            const spatial::bounds region{
                {std::min(origin.x, end.x), std::min(origin.y, end.y), std::min(origin.z, end.z)},
                {std::max(origin.x, end.x), std::max(origin.y, end.y), std::max(origin.z, end.z)}
            };
            index.query(
                region,
                [&](std::uint32_t id)
                {
                    const auto& t = triangles[id];
                    const auto edge1 = t.b - t.a, edge2 = t.c - t.a;
                    const auto h = cross(direction, edge2);
                    const float determinant = dot(edge1, h);
                    if (std::abs(determinant) < 1e-7f)
                    {
                        return;
                    }
                    const float inverse = 1.0f / determinant;
                    const auto s = origin - t.a;
                    const float u = inverse * dot(s, h);
                    if (u < 0 || u > 1)
                    {
                        return;
                    }
                    const auto q = cross(s, edge1);
                    const float v = inverse * dot(direction, q);
                    if (v < 0 || u + v > 1)
                    {
                        return;
                    }
                    const float d = inverse * dot(edge2, q);
                    if (d < 0.0001f || d >= distance)
                    {
                        return;
                    }
                    distance = d;
                    auto normal = normalize(cross(edge1, edge2));
                    if (dot(normal, direction) > 0)
                    {
                        normal = normal * -1.0f;
                    }
                    nearest = {true, origin + direction * d, normal, t.instance, t.index};
                }
            );
            return nearest;
        }
    };
} // namespace weapon
