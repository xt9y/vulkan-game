#include "camera.hpp"

#include "clt/player/prediction.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace clt::camera
{
    using namespace vk::util::math;
    namespace
    {
        std::mutex camera_mutex;
    } // namespace

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
        std::lock_guard<std::mutex> lock(camera_mutex);
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
        std::lock_guard<std::mutex> lock(camera_mutex);
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
        std::lock_guard<std::mutex> lock(camera_mutex);
        return perspective(get()->fov, aspect, 0.1f, 1000.0f);
    }
} // namespace clt::camera
