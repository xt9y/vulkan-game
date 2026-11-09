#pragma once
#include <vk/vk.hpp>

namespace clt::camera {
    struct state {
        float yaw = 0;
        float pitch = 0;
        float fov = 0;
        float sensitivity = 0;
    };
    struct frame {
        state pose;
        vk::util::math::mat4 view;
        vk::util::math::vec3 eye;
    };
    state* get();
    void init();
    void look(float dx, float dy);
    void set_look_sensitivity(float base,float ads_multiplier);
    void set_ads_fraction(float fraction);
    state snapshot();
    frame capture();
    void kick(float radians);
    void aim(float fraction);
    vk::util::math::mat4 view_projection(float aspect);

} // namespace clt::camera
