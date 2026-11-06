#pragma once

#include "srv/shared/scene/scene.hpp"
#include <spatial/index.hpp>

#include <cstdint>
#include <vector>

namespace player
{
    enum button : std::uint32_t {
        forward = 1u << 0,
        backward = 1u << 1,
        left = 1u << 2,
        right = 1u << 3,
        sprint = 1u << 4
    };

    inline constexpr std::uint32_t valid_buttons = 31;

    struct input {
        std::uint32_t sequence = 0;
        std::uint32_t buttons = 0;
        float yaw = 0;
    };

    struct parameters_t {
        vk::util::math::vec3 spawn;
        float walk_speed, sprint_speed, gravity, radius, height;
    };

    void configure(parameters_t values);
    const parameters_t& parameters();

    struct state {
        vk::util::math::vec3 position{};
        vk::util::math::vec3 velocity{};
        bool grounded = false;
    };

    struct box {
        vk::util::math::vec3 min;
        vk::util::math::vec3 max;
    };

    using collision_world = std::vector<box>;
    
    spatial::index build_index(const collision_world& world);
    collision_world build_colliders(const ::scene::state& scene);
    collision_world build_colliders(
        const std::vector<::scene::mesh>& meshes,
        const std::vector<::scene::instance>& instances
    );
    void step(state& player, const input& input, const collision_world& world, const spatial::index* broadphase = nullptr);
} // namespace player
