#include "movement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <numeric>

namespace player
{
    namespace
    {
        using namespace vk::util::math;

        constexpr float radius = 0.3f;
        constexpr float body_height = 1.8f;

        bool overlaps_xz(vec3 p, const box& b)
        {
            return p.x + radius > b.min.x && p.x - radius < b.max.x &&
                   p.z + radius > b.min.z && p.z - radius < b.max.z;
        }

        bool overlaps_y(float y, const box& b)
        {
            return y < b.max.y && y + body_height > b.min.y;
        }
    } // namespace

    collision_world build_colliders(const ::scene::state& scene)
    {
        return build_colliders(scene.meshes, scene.instances);
    }

    collision_world build_colliders(
        const std::vector<::scene::mesh>& meshes,
        const std::vector<::scene::instance>& instances
    )
    {
        using namespace vk::util::math;

        collision_world result;
        result.reserve(instances.size());

        for (const auto& instance : instances)
        {
            const auto mesh = std::find_if(
                meshes.begin(),
                meshes.end(),
                [&](const ::scene::mesh& value)
                {
                    return value.id == instance.mesh_id;
                }
            );

            if (mesh == meshes.end() || mesh->vertices.empty())
            {
                continue;
            }

            const float limit = std::numeric_limits<float>::max();
            box b{{limit, limit, limit}, {-limit, -limit, -limit}};

            for (const auto& vertex : mesh->vertices)
            {
                const auto& pose = instance.pose;

                const vec3 scaled{
                    vertex.position.x * pose.scale.x,
                    vertex.position.y * pose.scale.y,
                    vertex.position.z * pose.scale.z
                };

                const vec3 p = pose.position + rotate(pose.rotation, scaled);

                b.min.x = std::min(b.min.x, p.x);
                b.min.y = std::min(b.min.y, p.y);
                b.min.z = std::min(b.min.z, p.z);

                b.max.x = std::max(b.max.x, p.x);
                b.max.y = std::max(b.max.y, p.y);
                b.max.z = std::max(b.max.z, p.z);
            }

            result.push_back(b);
        }

        return result;
    }

    spatial::index build_index(const collision_world &world)
    {
        std::vector<spatial::bounds> boxes;
        boxes.reserve(world.size());

        for (const auto& b : world) 
        {
            boxes.push_back({b.min, b.max});
        }

        spatial::index result;
        result.rebuild(std::move(boxes));
        return result;
    }

    void step(state& p, const input& input, const collision_world& world, const spatial::index* broadphase) 
    {
        using namespace vk::util::math;

        const vec3 front{
            std::sin(input.yaw), 0, -std::cos(input.yaw)
        };

        const vec3 side{
            std::cos(input.yaw), 0, std::sin(input.yaw)
        };

        vec3 direction{};

        if (input.buttons & forward) direction = direction + front;
        if (input.buttons & backward) direction = direction - front;
        if (input.buttons & right) direction = direction + side;
        if (input.buttons & left) direction = direction - side;

        if (length_squared(direction) > 1.0f)
        {
            direction = normalize(direction);
        }

        const float speed = input.buttons & sprint ? 8.0f : 5.0f;

        p.velocity.x = direction.x * speed;
        p.velocity.z = direction.z * speed;

        const vec3 previous = p.position;

        std::vector<std::uint32_t> candidates;
        if (broadphase)
        {
            const float horizontal = radius + 8.0f * dt + 0.01f;
            const float vertical = std::abs(p.velocity.y) * dt + 20.0f * dt * dt + 0.05f;
            
            const spatial::bounds region{
                {
                    p.position.x - horizontal,
                    p.position.y - vertical,
                    p.position.z - horizontal
                },
                {
                    p.position.x + horizontal,
                    p.position.y + 1.8f + vertical,
                    p.position.z + horizontal
                }
            };

            broadphase->query(region, [&](std::uint32_t id){
                candidates.push_back(id);
            });
        }
        else
        {
            candidates.resize(world.size());
            std::iota(candidates.begin(), candidates.end(), 0u);
        }

        p.position.x += p.velocity.x * dt;

        for (const auto id : candidates)
        {
            const auto& b = world[id];
            if (!overlaps_y(p.position.y, b) || !overlaps_xz(p.position, b))
            {
                continue;
            }
            if (p.velocity.x > 0 && previous.x + radius <= b.min.x)
            {
                p.position.x = b.min.x - radius;
            }
            else if (p.velocity.x < 0 && previous.x - radius >= b.max.x)
            {
                p.position.x = b.max.x + radius;
            }
        }

        const float previous_z = p.position.z;
        p.position.z += p.velocity.z * dt;

        for (const auto id : candidates)
        {
            const auto& b = world[id];
            if (!overlaps_y(p.position.y, b) || !overlaps_xz(p.position, b))
            {
                continue;
            }

            if (p.velocity.z > 0 && previous_z + radius <= b.min.z)
            {
                p.position.z = b.min.z - radius;
            }
            else if (p.velocity.z < 0 && previous_z - radius >= b.max.z)
            {
                p.position.z = b.max.z + radius;
            }
        }

        p.velocity.y -= 20.0f * dt;
        p.position.y += p.velocity.y * dt;
        p.grounded = false;

        for (const auto id : candidates)
        {
            const auto& b = world[id];
            if (!overlaps_xz(p.position, b))
            {
                continue;
            }

            if (p.velocity.y <= 0 && previous.y >= b.max.y - 0.0001f && p.position.y < b.max.y)
            {
                p.position.y = b.max.y;
                p.velocity.y = 0;
                p.grounded = true;
            }
            else if (p.velocity.y > 0 && previous.y + body_height <= b.min.y && p.position.y + body_height > b.min.y)
            {
                p.position.y = b.min.y - body_height;
                p.velocity.y = 0;
            }
        }
    }
} // namespace player
