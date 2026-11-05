#pragma once

#include "srv/shared/weapon/weapon.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vk/vk.hpp>

namespace weapon
{
    enum class motion
    {
        idle,walk,draw,reload,reload_empty,
        ads_idle,ads_in,ads_out,ads_walk,
        fire1,fire2,fire3,ads_fire1,ads_fire2,ads_fire3
    };

    struct clip
    {
        std::string name;
        float speed=1.f;
        bool loop=false;
    };

    // GLFW-compatible IDs are supplied by game code. This shared header
    // has no window/GLFW dependency and is safe for dedicated servers.
    struct bindings
    {
        int trigger_button=0,aim_button=1;
        int reload_key='R',equip_key=-1;
        bool automatic=false;
        float sensitivity=.0025f;
        float ads_sensitivity_multiplier=.60f;
    };

    // One complete description per gun; all authoritative settings and
    // client presentation remain together in game-owned weapons.cpp.
    struct gun
    {
        definition rules;
        bindings input{};
        std::filesystem::path model,casing_model;
        std::map<motion,clip> animations;
        std::string camera_joint,muzzle_joint,ejection_joint,slide_joint;
        vk::util::math::vec3 muzzle_offset{0.f,0.f,-.4f};
        vk::util::math::vec3 ejection_offset{.08f,-.04f,-.12f};
        vk::util::math::mat4 correction=vk::util::math::mat4::identity();
        float fov=1.2f,ads_fov=1.2f,near_plane=.01f,far_plane=20.f;
        float ads_zoom=0.f,ads_speed=14.f;
        float slide_offset=0.f;
        float recoil=0.f,hip_kick=0.f,ads_kick=0.f;
        float recoil_decay=15.f;
        vk::util::math::vec3 casing_impulse{1.f,1.f,0.f};
        float casing_lifetime=9.f,casing_spin=14.f;
        std::uint32_t max_casings=48;
        float flash_duration=.035f;
        vk::util::math::vec3 flash_scale{.035f,.045f,.035f};
        float impact_radius=.009f,impact_offset=.0008f,impact_lifetime=120.f;
        std::uint32_t max_impacts=192;
    };
}
