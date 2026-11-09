#pragma once
#include "animation.hpp"
#include "../server/weapon.hpp"
#include "../server/scene.hpp"
#include <vk/vk.hpp>
#include <cstdint>
#include <vector>

namespace clt::weapon {
    using motion=::weapon::motion;
    using clip=::weapon::clip;
    using bindings=::weapon::bindings;
    using definition=::weapon::gun;
    using gun=::weapon::gun;
    bindings active_bindings();
    void register_weapon(definition value);
    void check_magazine();
    struct instance_data {
        vk::util::math::mat4 model;
        vk::util::math::mat4 normal;
    };
    struct draw {
        std::uint32_t mesh_id = 0;
        bool viewmodel = false;
        std::vector<instance_data> instances;
        std::vector<animation::matrix> palette;
    };
    void controls(bool trigger, bool ads, bool reload, std::uint32_t movement);
    void initialize();
    const ::scene::state& assets();
    const std::vector<draw>& update(const vk::util::math::mat4& view);
    vk::util::math::mat4 view_projection(float aspect, const vk::util::math::mat4& view);
    void shutdown();

} // namespace clt::weapon
