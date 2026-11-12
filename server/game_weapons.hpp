#pragma once
#include "weapon.hpp"

namespace game_weapons
{
::weapon::gun make()
{
    ::weapon::gun glock{};
    glock.rules.id = 1;
    glock.rules.magazine_capacity = 15;
    glock.rules.initial_reserve = 90;
    glock.rules.fire_interval = .25;
    glock.rules.range = 200;
    glock.rules.reload = weapon::load_timing("assets/glock/metadata.json", "Glock_Reload", "Glock_ReloadEmpty", 20, 20);
    glock.input.ads_sensitivity_multiplier = .90f;
    glock.model = "assets/glock/glock.glb";
    glock.casing_model = "assets/props/casing.glb";
    glock.camera_joint = "camera";
    glock.muzzle_joint = "Muzzle";
    glock.ejection_joint = "EjectionPort";
    glock.slide_joint = "Slide";
    glock.animations = {{{"Glock_Idle", 1, true}, {"Glock_Walk", 1, true}, {"Glock_Draw", 1, false}, {"Glock_Reload", 1, false}, {"Glock_ReloadEmpty", 1, false}, {"Glock_ADS_Idle", 1, true}, {"Glock_ADS_In", 3, false}, {"Glock_ADS_Out", 3, false}, {"Glock_ADS_Walk", 1, true}, {"Glock_Fire1", 1.5f, false}, {"Glock_Fire2", 1.5f, false}, {"Glock_Fire3", 1.5f, false}, {"Glock_ADS_Fire1", 1.5f, false}, {"Glock_ADS_Fire2", 1.5f, false}, {"Glock_ADS_Fire3", 1.5f, false}}};
    glock.fov = 1.1f;
    glock.ads_fov = glock.fov - .58f;
    glock.ads_zoom = .58f;
    glock.ads_speed = 11.f;
    glock.slide_offset = .025f;
    glock.recoil = .022f;
    glock.hip_kick = .015f;
    glock.ads_kick = .009f;
    glock.casing_impulse = {1.6f, 1.5f, .35f};
    glock.impact_radius = .009f;

    return glock;
}

} // namespace game_weapons
