#pragma once

#include <vk/vk.hpp>

namespace clt::camera
{
    struct state {
        float yaw = 0;
        float pitch = 0;
        float fov = 0;
        float sensitivity = 0;
    };

    struct frame
    {
        state pose;
        vk::util::math::mat4 view;
    };

    state* get();
    void init();
    void look(float dx, float dy);
    // Updated when the active gun changes; ADS sensitivity is interpolated
    // using the same aim fraction as the camera's zoom.
    void set_look_sensitivity(float base,float ads_multiplier);
    state snapshot();
    // Freeze mouse look, recoil and predicted eye position for one render frame.
    frame capture();
    void kick(float radians);
    void aim(float fraction);
    vk::util::math::mat4 view_projection(float aspect);

    vk::util::math::mat4 view();
    vk::util::math::mat4 projection(float aspect);
} // namespace clt::camera
