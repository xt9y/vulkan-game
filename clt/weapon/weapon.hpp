#pragma once
#include "clt/animation/animation.hpp"
#include "srv/shared/weapon/weapon.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <vk/vk.hpp>

namespace clt::weapon
{
    enum class motion
    {
        idle,
        walk,
        draw,
        reload,
        reload_empty,
        ads_idle,
        ads_in,
        ads_out,
        ads_walk,
        fire1,
        fire2,
        fire3,
        ads_fire1,
        ads_fire2,
        ads_fire3
    };

    struct clip
    {
        std::string name;
        float speed = 1;
        bool loop = false;
    };

    // GLFW button/key IDs are supplied by the game. The engine only
    // forwards these bindings; no pistol-specific keys are hardcoded.
    struct bindings
    {
        int trigger_button=0, aim_button=1;
        int reload_key=82, equip_key=-1;
        bool automatic=false;
        float sensitivity=.0025f;
        float ads_sensitivity_multiplier=.60f;
    };

    struct definition
    {
        ::weapon::definition rules;
        bindings input{};
        std::filesystem::path model;
        std::filesystem::path casing_model;
        std::map<motion, clip> animations;
        std::string camera_joint, muzzle_joint, ejection_joint, slide_joint;
        // Optional fallback sockets when an animated model lacks named joints.
        vk::util::math::vec3 muzzle_offset{0.f,0.f,-.4f};
        vk::util::math::vec3 ejection_offset{.08f,-.04f,-.12f};
        vk::util::math::mat4 correction = vk::util::math::mat4::identity();
        float fov = 1.2f, ads_fov = 1.2f, near_plane = .01f, far_plane = 20;
        float ads_zoom = 0, ads_speed = 14;
        float slide_offset = 0;
        float recoil = 0, hip_kick = 0, ads_kick = 0;
        float recoil_decay = 15;
        vk::util::math::vec3 casing_impulse{1, 1, 0};
        float casing_lifetime = 9, casing_spin = 14;
        std::uint32_t max_casings = 48;
        float flash_duration = .035f;
        vk::util::math::vec3 flash_scale{.035f, .045f, .035f};
        float impact_radius = .009f, impact_offset = .0008f, impact_lifetime = 120;
        std::uint32_t max_impacts = 192;
    };

    // Complete game-facing gun profile: authoritative rules, controls,
    // camera/ADS, models, attachment joints, animations and effects.
    using gun=definition;

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
