#pragma once

#include "srv/shared/scene/scene.hpp"
#include "srv/shared/spatial/index.hpp"

#include <cstdint>
#include <vector>

namespace player
{
    inline constexpr float dt = 1.0f / 60.0f;

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

    struct state {
        vk::util::math::vec3 position{0, 0, 4};
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
    void step(state& player, const input& input, const collision_world& world, const spatial::index* broadphase = nullptr);
} // namespace player
