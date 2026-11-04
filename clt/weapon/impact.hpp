#pragma once

#include "srv/shared/net/impact.hpp"
#include "srv/shared/scene/scene.hpp"
#include "srv/shared/weapon/weapon.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace clt::impact
{
    using vk::util::math::vec3;
    using vk::util::math::mat4;

    struct drop
    {
        vec3 position{}, velocity{};
        float radius = .01f, age = 0, lifetime = 2;
        std::uint32_t seed = 1;
        ::weapon::impact_type type = ::weapon::impact_type::flesh;
    };

    struct stain
    {
        mat4 transform = mat4::identity();
        float age = 0, lifetime = 300;
        std::uint32_t variant = 0;
    };

    struct pool
    {
        vec3 position{}, normal{};
        float radius = .035f, final_radius = .2f, age = 0, angle = 0;
        std::uint32_t variant = 0;
    };

    struct vat_animation
    {
        mat4 transform = mat4::identity();
        std::uint32_t type = 19;
        float age = 0, lifetime = 2.5f;
        bool mirror = false;
    };

    struct system
    {
        std::vector<drop> drops;
        std::deque<stain> stains;
        std::deque<pool> pools;
        std::deque<vat_animation> vat_animations;

        void spawn(const net::impact::event& effect);
        void update(const ::weapon::world& collision, float dt);
    };

    mat4 decal_transform(vec3 position, vec3 normal, float size, float angle);
    mat4 drop_transform(vec3 position, float radius);
    // Local effects use the existing instanced PBR scene renderer and texture cache.
    void append_assets(::scene::state& scene,
                       std::uint32_t first_decal,
                       std::uint32_t drop_mesh);
} // namespace clt::impact
