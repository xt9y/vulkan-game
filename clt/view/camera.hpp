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

    state* get();
    void init();
    void look(float dx, float dy);
    state snapshot();
    void set_fov(float value);
    void set_sensitivity(float value);
    vk::util::math::mat4 view_projection(float aspect);

    vk::util::math::mat4 view();
    vk::util::math::mat4 projection(float aspect);
} // namespace clt::camera
