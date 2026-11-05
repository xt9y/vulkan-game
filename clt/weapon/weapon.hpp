#pragma once
#include "clt/animation/animation.hpp"
#include "srv/shared/weapon/gun.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <vk/vk.hpp>

namespace clt::weapon
{
    // Shared API: the server consumes gun.rules, the client consumes its
    // model, input, camera, animation and presentation configuration.
    using motion=::weapon::motion;
    using clip=::weapon::clip;
    using bindings=::weapon::bindings;
    using definition=::weapon::gun;
    using gun=::weapon::gun;

    struct equip_binding
    {
        std::uint32_t id=0;
        int key=-1;
    };
    bindings active_bindings();
    std::vector<equip_binding> equip_bindings();

    // Game-owned declarations, registered before renderer initialization.
    void register_weapon(definition value);
    // Queued from the input thread; server validates the requested weapon ID.
    void equip(std::uint32_t id);

    struct instance_data
    {
        vk::util::math::mat4 model;
        vk::util::math::mat4 normal;
    };

    struct draw
    {
        std::uint32_t mesh_id = 0;
        bool viewmodel = false;
        std::vector<instance_data> instances;
        std::vector<animation::matrix> palette;
    };

    // Input-thread producer; edge commands are retained until consumed by the render thread.
    void controls(bool trigger, bool ads, bool reload, std::uint32_t movement);
    void initialize();
    const ::scene::state& assets();
    // Placement and projection must use the same captured render-frame view.
    const std::vector<draw>& update(const vk::util::math::mat4& view);
    vk::util::math::mat4 view_projection(float aspect, const vk::util::math::mat4& view);

    struct status
    {
        std::uint32_t magazine = 0, reserve = 0;
        bool reloading = false;
        float ads = 0;
        std::string animation;
        std::size_t casings = 0, impacts = 0;
        std::uint32_t weapon_id = 0;
    };

    // Render-thread view of the predicted HUD and presentation state.
    status inspect();
    void shutdown();
} // namespace clt::weapon
