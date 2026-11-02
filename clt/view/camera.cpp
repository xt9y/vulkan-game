#include "camera.hpp"

#include "clt/network/connection.hpp"
#include "clt/player/prediction.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace clt::camera
{
    using namespace vk::util::math;
    namespace
    {
        std::mutex camera_mutex;
        float recoil = 0, aim_fraction = 0;
        std::chrono::steady_clock::time_point recoil_time{};

        float current_recoil()
        {
            return recoil *
                   std::exp(
                       -12.f *
                       std::chrono::duration<float>(std::chrono::steady_clock::now() - recoil_time)
                           .count()
                   );
        }

        vec3 eye_position()
        {
            const auto parameters = connection::movement_parameters();
            return prediction::position() + vec3{0, parameters ? parameters->height : 0.0f, 0};
        }
    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        *get() = {};
        recoil = aim_fraction = 0;
        recoil_time = std::chrono::steady_clock::now();
        get()->fov = 1.04719755f;
        get()->sensitivity = 0.0025f;
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

    state snapshot()
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        auto result = *get();
        result.pitch = std::clamp(result.pitch + current_recoil(), -1.55f, 1.55f);
        result.fov *= 1.f - aim_fraction;
        return result;
    }

    void kick(float radians)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        recoil = std::min(current_recoil() + radians, .08f);
        recoil_time = std::chrono::steady_clock::now();
    }

    void aim(float fraction)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        aim_fraction = std::clamp(fraction, 0.f, .95f);
    }

    frame capture()
    {
        const auto camera = snapshot();
        const vec3 eye = eye_position();
        const vec3 direction{
            std::sin(camera.yaw) * std::cos(camera.pitch),
            std::sin(camera.pitch),
            -std::cos(camera.yaw) * std::cos(camera.pitch)
        };
        return {camera, look_at(eye, eye + direction)};
    }

    mat4 view()
    {
        return capture().view;
    }

    mat4 projection(float aspect)
    {
        return perspective(snapshot().fov, aspect, 0.1f, 1000.0f);
    }

    mat4 view_projection(float aspect)
    {
        const auto frame = capture();
        return perspective(frame.pose.fov, aspect, 0.1f, 1000.0f) * frame.view;
    }
} // namespace clt::camera
