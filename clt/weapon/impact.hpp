#pragma once

#include "srv/shared/net/impact.hpp"
#include "srv/shared/scene/scene.hpp"
#include "srv/shared/weapon/weapon.hpp"

#include <vk/util/math/matrix.hpp>

#include <cstdint>
#include <deque>
#include <vector>

namespace clt::impact
{
    using vk::util::math::vec3;
    using vk::util::math::mat4;

    // Only flesh effects exist here. Other impact types use ordinary bullet marks.
    struct drop
    {
        vec3 position{}, velocity{};
        float radius = .012f, age = 0, lifetime = 1.5f;
        std::uint32_t seed = 1;
    };

    struct stain
    {
        mat4 transform = mat4::identity();
        mat4 local = mat4::identity();
        std::uint32_t anchor = 0;
        std::uint32_t variant = 0;
        float age = 0, lifetime = 240;
        float wetness = 1;
    };

    struct pool
    {
        vec3 position{}, normal{};
        float radius = .08f, final_radius = .4f;
        float age = 0, angle = 0, lifetime = 240;
        std::uint32_t variant = 0, anchor = 0;
        mat4 local = mat4::identity();
    };

    struct projector
    {
        mat4 transform = mat4::identity();
        std::uint32_t variant = 0;
        float wetness = 1;
        bool growing_pool = false;
    };

    struct wound
    {
        std::uint32_t target = 0;
        vk::util::math::vec2 uv{};
        float radius = .04f, strength = 1;
        float age = 0;
        float angle = 0;
        vec3 position{};
        vk::util::math::vec4 local{};
        float next_drip = 1;
        std::uint32_t seed = 1;
    };

    struct backdrop
    {
        vec3 position{}, direction{};
        float strength = 1;
        std::uint32_t seed = 1;
        std::uint32_t target = 0;
    };

    struct system
    {
        std::vector<drop> drops;
        std::deque<stain> stains;
        std::deque<pool> pools;
        std::deque<wound> wounds;
        std::vector<backdrop> pending_backdrops;

        void spawn(const net::impact::event& effect);
        void update(const ::weapon::world& collision, float dt);
        std::vector<projector> projectors(const ::scene::state* scene) const;
    };

    mat4 decal_transform(vec3 position, vec3 normal, float size, float angle);
    mat4 drop_transform(vec3 position, float radius, vec3 velocity = {});
    inline constexpr std::uint32_t decal_texture_id = 0x600f0000u;

    void append_assets(::scene::state& scene,
                       std::uint32_t first_decal,
                       std::uint32_t drop_mesh);
} // namespace clt::impact
