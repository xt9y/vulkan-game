#pragma once
#include "net/limits.hpp"
#include "scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <spatial/index.hpp>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace player
{
enum button : std::uint32_t
{
    forward = 1u << 0,
    backward = 1u << 1,
    left = 1u << 2,
    right = 1u << 3,
    sprint = 1u << 4
};

inline constexpr std::uint32_t valid_buttons = 31;
struct input
{
    std::uint32_t sequence = 0;
    std::uint32_t buttons = 0;

    float yaw = 0;
};

struct parameters_t
{
    vk::util::math::vec3 spawn;
    float walk_speed, sprint_speed, gravity, radius, height;
};

void configure(parameters_t values);
const parameters_t &parameters();
struct state
{
    vk::util::math::vec3 position{};
    vk::util::math::vec3 velocity{};

    bool grounded = false;
};

struct box
{
    vk::util::math::vec3 min;
    vk::util::math::vec3 max;
};

using collision_world = std::vector<box>;
spatial::index build_index(const collision_world &world);
collision_world build_colliders(const ::scene::state &scene);
collision_world build_colliders(const std::vector<::scene::mesh> &meshes, std::span<const ::scene::instance> instances);
void step(state &player, const input &input, const collision_world &world, const spatial::index *broadphase = nullptr);
namespace
{
using namespace vk::util::math;
std::optional<parameters_t> configured;
bool overlaps_xz(vec3 p, const box &b)
{
    const float r = parameters().radius;

    return p.x + r > b.min.x && p.x - r < b.max.x && p.z + r > b.min.z && p.z - r < b.max.z;
}

bool overlaps_y(float y, const box &b)
{
    return y < b.max.y && y + parameters().height > b.min.y;
}
} // namespace
void configure(parameters_t p)
{
    if (!std::isfinite(p.spawn.x) || !std::isfinite(p.spawn.y) || !std::isfinite(p.spawn.z) || !std::isfinite(p.walk_speed) || !std::isfinite(p.sprint_speed) || !std::isfinite(p.gravity) ||
        !std::isfinite(p.radius) || !std::isfinite(p.height) || p.walk_speed <= 0 || p.sprint_speed <= 0 || p.gravity <= 0 || p.radius <= 0 || p.height <= 0)
        throw std::invalid_argument("Player parameters must be specified by the server");
    configured = p;
}

const parameters_t &parameters()
{
    if (!configured) throw std::logic_error("Player parameters not yet received from server");

    return *configured;
}

collision_world build_colliders(const ::scene::state &scene)
{
    return build_colliders(scene.meshes, scene.instances);
}

collision_world build_colliders(const std::vector<::scene::mesh> &meshes, std::span<const ::scene::instance> instances)
{
    using namespace vk::util::math;

    collision_world boxes;
    boxes.reserve(instances.size());

    std::unordered_map<std::uint32_t,const ::scene::mesh*> by_id;
    by_id.reserve(meshes.size());
    for(const auto& mesh:meshes)by_id.emplace(mesh.id,&mesh);
    for (const auto &instance : instances)
    {
        const auto it=by_id.find(instance.mesh_id);
        if(it==by_id.end() || it->second->vertices.empty())continue;
        const auto& mesh=*it->second;

        const float limit = std::numeric_limits<float>::max();
        box b{{limit, limit, limit}, {-limit, -limit, -limit}};
        const auto &pose = instance.pose;

        for (const auto &v : mesh->vertices)
        {
            const vec3 scaled{
                v.position.x * pose.scale.x, v.position.y * pose.scale.y, v.position.z * pose.scale.z
            };

            const vec3 p = pose.position + rotate(pose.rotation, scaled);
            b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
            b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
        }

        boxes.push_back(b);
    }

    return boxes;
}

std::vector<spatial::bounds> collision_bounds(const collision_world &boxes)
{
    std::vector<spatial::bounds> result;
    result.reserve(boxes.size());

    for (const auto &box : boxes)
        result.push_back({box.min, box.max});

    return result;
}

spatial::index build_index(const collision_world &boxes)
{
    spatial::index index;
    index.rebuild(collision_bounds(boxes));

    return index;
}

void step(state &p, const input &keys, const collision_world &world, const spatial::index *broadphase)
{
    using namespace vk::util::math;
    const auto &settings = parameters();

    const float radius = settings.radius, height = settings.height;
    const float dt = 1.f / float(net::limits::tick_hz);

    const vec3 front{std::sin(keys.yaw), 0, -std::cos(keys.yaw)};
    const vec3 side{std::cos(keys.yaw), 0, std::sin(keys.yaw)};
    vec3 movement{};

    if (keys.buttons & forward) movement = movement + front;
    if (keys.buttons & backward) movement = movement - front;
    if (keys.buttons & right) movement = movement + side;
    if (keys.buttons & left) movement = movement - side;
    if (length_squared(movement) > 1.f) movement = normalize(movement);

    const float speed = keys.buttons & sprint ? settings.sprint_speed : settings.walk_speed;
    p.velocity.x = movement.x * speed;
    p.velocity.z = movement.z * speed;

    const vec3 previous = p.position;

    thread_local std::vector<std::uint32_t> candidates;
    candidates.clear();

    if (broadphase) {
        const float horizontal = radius + std::max(settings.walk_speed, settings.sprint_speed) * dt + .01f;
        const float vertical = std::abs(p.velocity.y) * dt + settings.gravity * dt * dt + .05f;
        broadphase->query({{p.position.x - horizontal, p.position.y - vertical, p.position.z - horizontal}, {p.position.x + horizontal, p.position.y + height + vertical, p.position.z + horizontal}}, [&](std::uint32_t id)
                          { candidates.push_back(id); });
    }
    else {
        candidates.resize(world.size());
        std::iota(candidates.begin(), candidates.end(), 0u);
    }

    for (int axis = 0; axis < 2; ++axis)
    {
        float &position = axis ? p.position.z : p.position.x;

        const float velocity = axis ? p.velocity.z : p.velocity.x;
        const float old = position;
        position += velocity * dt;

        for (const auto id : candidates)
        {
            const auto &b = world[id];

            if (!overlaps_y(p.position.y, b) || !overlaps_xz(p.position, b)) continue;

            const float lo = axis ? b.min.z : b.min.x;
            const float hi = axis ? b.max.z : b.max.x;

            if (velocity > 0 && old + radius <= lo) position = lo - radius;
            else if (velocity < 0 && old - radius >= hi)
                position = hi + radius;
        }
    }

    p.velocity.y -= settings.gravity * dt;
    p.position.y += p.velocity.y * dt;
    p.grounded = false;

    for (const auto id : candidates)
    {
        const auto &b = world[id];

        if (!overlaps_xz(p.position, b)) continue;
        if (p.velocity.y <= 0 && previous.y >= b.max.y - .0001f && p.position.y < b.max.y) {
            p.position.y = b.max.y;
            p.velocity.y = 0;
            p.grounded = true;
        }
        else if (p.velocity.y > 0 && previous.y + height <= b.min.y && p.position.y + height > b.min.y) {
            p.position.y = b.min.y - height;
            p.velocity.y = 0;
        }
    }
}

} // namespace player
