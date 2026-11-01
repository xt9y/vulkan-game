#include "camera.hpp"

#include "clt/player/prediction.hpp"

#include <algorithm>
#include <cmath>

namespace clt::camera
{
    using namespace vk::util::math;

    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        *get() = {};
    }

    void look(float dx, float dy)
    {
        auto& camera = *get();

        camera.yaw = std::remainder(
            camera.yaw + dx * camera.sensitivity,
            6.283185307f
        );

        camera.pitch = std::clamp(
            camera.pitch - dy * camera.sensitivity,
            -1.55f,
            1.55f
        );
    }

    mat4 view()
    {
        const auto& camera = *get();

        const vec3 eye = prediction::position() + vec3{0, 1.65f, 0};

        const vec3 direction{
            std::sin(camera.yaw) * std::cos(camera.pitch),
            std::sin(camera.pitch),
            -std::cos(camera.yaw) * std::cos(camera.pitch)
        };

        return look_at(eye, eye + direction);
    }

    mat4 projection(float aspect)
    {
        return perspective(get()->fov, aspect, 0.1f, 1000.0f);
    }
} // namespace clt::camera
