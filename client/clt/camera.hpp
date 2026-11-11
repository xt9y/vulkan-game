#pragma once
#include "connection.hpp"
#include "prediction.hpp"
#include <chrono>
#include <cmath>
#include <mutex>
#include <vk/vk.hpp>

namespace clt::camera
{
struct state
{
    float yaw = 0;
    float pitch = 0;
    float fov = 0;
    float sensitivity = 0;
};

struct frame
{
    state pose;

    vk::util::math::mat4 view;
    vk::util::math::vec3 eye;
};

state *get();
void init();
void reset_view();
void look(float dx, float dy);
void set_look_sensitivity(float base, float ads_multiplier);
void set_ads_fraction(float fraction);
state snapshot();
frame capture();
void kick(float radians);
void aim(float fraction);
vk::util::math::mat4 view_projection(float aspect);

using namespace vk::util::math;
namespace
{
using clock = std::chrono::steady_clock;

state data{};
std::mutex guard;

float recoil = 0, zoom = 0, ads = 0, sensitivity_multiplier = 1;

clock::time_point recoil_time{};
float decayed_recoil()
{
    return recoil * std::exp(-12.f * std::chrono::duration<float>(clock::now() - recoil_time).count());
}
} // namespace
state *get() { return &data; }
void init()
{
    std::lock_guard lock(guard);
    data = {};
    data.fov = 1.04719755f;
    data.sensitivity = .0025f;
    recoil = zoom = ads = 0;
    sensitivity_multiplier = 1;
    recoil_time = clock::now();
}

void reset_view()
{
    std::lock_guard lock(guard);
    data.yaw=data.pitch=0;
    recoil=zoom=ads=0;
    recoil_time=clock::now();
}
void set_look_sensitivity(float base, float multiplier)
{
    std::lock_guard lock(guard);
    data.sensitivity = std::clamp(base, .00001f, .1f);
    sensitivity_multiplier = std::clamp(multiplier, .05f, 1.f);
}

void set_ads_fraction(float fraction)
{
    std::lock_guard lock(guard);
    ads = std::clamp(fraction, 0.f, 1.f);
}

void look(float dx, float dy)
{
    std::lock_guard lock(guard);

    const float speed = data.sensitivity * (1.f - ads * (1.f - sensitivity_multiplier));
    data.yaw = std::remainder(data.yaw + dx * speed, 6.283185307f);
    data.pitch = std::clamp(data.pitch - dy * speed, -1.55f, 1.55f);
}

state snapshot()
{
    std::lock_guard lock(guard);

    auto pose = data;
    pose.pitch = std::clamp(pose.pitch + decayed_recoil(), -1.55f, 1.55f);
    pose.fov *= 1.f - zoom;

    return pose;
}

void kick(float amount)
{
    std::lock_guard lock(guard);
    recoil = std::min(decayed_recoil() + amount, .08f);
    recoil_time = clock::now();
}

void aim(float fraction)
{
    std::lock_guard lock(guard);
    zoom = std::clamp(fraction, 0.f, .95f);
}

frame capture()
{
    const auto pose = snapshot();
    const auto params = connection::movement_parameters();
    const vec3 eye = prediction::position() + vec3{
                                                  0.f, params ? params->height : 0.f, 0.f
                                              };

    const vec3 forward{
        std::sin(pose.yaw) * std::cos(pose.pitch), std::sin(pose.pitch), -std::cos(pose.yaw) * std::cos(pose.pitch)
    };

    return {pose, look_at(eye, eye + forward), eye};
}

mat4 view_projection(float aspect)
{
    const auto frame = capture();

    return perspective(frame.pose.fov, aspect, .1f, 1000.f) * frame.view;
}

} // namespace clt::camera
