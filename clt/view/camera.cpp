#include "camera.hpp"
#include "srv/game/options.hpp"

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
        std::lock_guard<std::mutex> lock(camera_mutex);
        *get() = {};
        get()->fov = game::options::get()->view.fov;
        get()->sensitivity = game::options::get()->view.sensitivity;
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
            -game::options::get()->view.pitch_limit,
            game::options::get()->view.pitch_limit
        );
    }

    state snapshot()
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        return *get();
    }

    void set_fov(float value)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        get()->fov = value;
        game::options::get()->view.fov = value;
    }

    void set_sensitivity(float value)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        get()->sensitivity = value;
        game::options::get()->view.sensitivity = value;
    }

    mat4 view()
    {
        const auto camera = snapshot();
        const vec3 eye = prediction::position() + vec3{0, game::options::get()->view.eye_height, 0};
        const vec3 direction{
            std::sin(camera.yaw) * std::cos(camera.pitch),
            std::sin(camera.pitch),
            -std::cos(camera.yaw) * std::cos(camera.pitch)
        };
        return look_at(eye, eye + direction);
    }

    mat4 projection(float aspect)
    {
        return perspective(snapshot().fov, aspect, game::options::get()->view.near_plane, game::options::get()->view.far_plane);
    }

    mat4 view_projection(float aspect)
    {
        const auto camera = snapshot();
        const vec3 eye = prediction::position() + vec3{0, game::options::get()->view.eye_height, 0};
        const vec3 direction{
            std::sin(camera.yaw) * std::cos(camera.pitch),
            std::sin(camera.pitch),
            -std::cos(camera.yaw) * std::cos(camera.pitch)
        };
        return perspective(camera.fov, aspect, game::options::get()->view.near_plane, game::options::get()->view.far_plane) *
            look_at(eye, eye + direction);
    }
} // namespace clt::camera
