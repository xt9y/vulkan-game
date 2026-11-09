#pragma once
#include "weapon.hpp"

namespace game_weapons
{
::weapon::gun make();

::weapon::gun make()
{
    using motion = ::weapon::motion;
    ::weapon::gun glock{};
    glock.rules.id = 1;
    glock.rules.magazine_capacity = 15;
    glock.rules.initial_reserve = 90;
    glock.rules.fire_interval = .25;
    glock.rules.range = 200;
    glock.rules.reload = weapon::load_timing("assets/glock/metadata.json", "Glock_Reload", "Glock_ReloadEmpty", 20, 20);
    glock.input.trigger_button = 0; // left mouse button
    glock.input.aim_button = 1;     // right mouse button
    glock.input.reload_key = 'R';
    glock.input.inspect_key = 'M'; // print magazine/reserve without reloading
    glock.input.automatic = false; // hold to fire at the server-validated interval
    glock.input.sensitivity = .0025f;
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
    glock.casing_lifetime = 9.f;
    glock.max_casings = 48;
    glock.flash_duration = .035f;
    glock.flash_scale = {.035f, .045f, .035f};
    glock.impact_radius = .009f;
    glock.impact_lifetime = 120.f;
    glock.max_impacts = 192;

    return glock;
}

} // namespace game_weapons
