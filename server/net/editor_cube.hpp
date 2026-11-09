#pragma once
#include "editor_light.hpp"
#include <algorithm>

namespace net::editor_cube
{
using kind = editor_light::kind;
inline constexpr std::size_t max_cubes = 1024;
inline constexpr std::array<std::string_view, 13> property_keys{
    "pos_x", "pos_y", "pos_z", "rot_x", "rot_y", "rot_z", "width", "height", "depth",
    "color_r", "color_g", "color_b", "texture"
};

struct resource
{
    kind type = kind::state;
    std::uint32_t token = 0, sequence = 0, index = 0, revision = 0;
    ::scene::instance cube{};
    float value = 0;
    bool enabled = true;
};
using packet = std::array<std::uint8_t, 96>;

inline vk::util::math::vec3 angles(const ::scene::instance &cube)
{
    using namespace vk::util::math;
    const auto q = normalize(cube.pose.rotation);
    constexpr float radians = .017453292519943295f;
    const auto &reference = cube.rotation_reference;
    const auto reference_q = euler(reference.x * radians, reference.y * radians, reference.z * radians);
    // Only use the UI reference if it describes the authoritative quaternion.
    if (std::abs(dot(q, reference_q)) > .9999999f) return reference;
    const float pitch = std::asin(std::clamp(2.f * (q.w * q.x - q.y * q.z), -1.f, 1.f));
    float yaw, roll;
    if (std::abs(std::cos(pitch)) > .0001f) {
        yaw = std::atan2(2.f * (q.x * q.z + q.w * q.y), 1.f - 2.f * (q.x * q.x + q.y * q.y));
        roll = std::atan2(2.f * (q.x * q.y + q.w * q.z), 1.f - 2.f * (q.x * q.x + q.z * q.z));
    }
    else {
        yaw = std::atan2(2.f * (q.w * q.y - q.x * q.z), 1.f - 2.f * (q.y * q.y + q.z * q.z));
        roll = 0;
    }
    constexpr float degrees = 57.29577951308232f;
    return {pitch * degrees, yaw * degrees, roll * degrees};
}

inline bool set_property(::scene::instance &cube, std::uint32_t property, float value)
{
    if (!std::isfinite(value) || property >= property_keys.size()) return false;
    if (property < 3) {
        if (std::abs(value) > 500.f) return false;
        std::array fields{&cube.pose.position.x, &cube.pose.position.y, &cube.pose.position.z};
        *fields[property] = value;
    }
    else if (property < 6) {
        if (std::abs(value) > 180.f) return false;
        auto a = angles(cube);
        std::array fields{&a.x, &a.y, &a.z};
        *fields[property - 3] = value;
        constexpr float radians = .017453292519943295f;
        cube.pose.rotation = vk::util::math::euler(a.x * radians, a.y * radians, a.z * radians);
        cube.rotation_reference = a;
    }
    else if (property < 9) {
        if (value < .01f || value > 100.f) return false;
        std::array fields{&cube.pose.scale.x, &cube.pose.scale.y, &cube.pose.scale.z};
        *fields[property - 6] = value;
    }
    else if (property < 12) {
        if (value < 0.f || value > 100.f) return false;
        std::array fields{&cube.tint.x, &cube.tint.y, &cube.tint.z};
        *fields[property - 9] = value / 100.f;
    }
    else {
        if (value != 0.f && value != 1.f) return false;
        cube.pattern = std::uint32_t(value);
    }
    return true;
}

inline bool valid(const ::scene::instance &cube)
{
    const auto &p = cube.pose;
    const auto coord = [](float v) { return std::isfinite(v) && std::abs(v) <= 500.f; };
    const auto dim = [](float v) { return std::isfinite(v) && v >= .01f && v <= 100.f; };
    const auto color = [](float v) { return std::isfinite(v) && v >= 0.f && v <= 1.f; };
    const float norm = vk::util::math::dot(p.rotation, p.rotation);
    return cube.mesh_id == ::scene::unit_cube_mesh_id && coord(p.position.x) && coord(p.position.y) && coord(p.position.z) &&
           dim(p.scale.x) && dim(p.scale.y) && dim(p.scale.z) && color(cube.tint.x) && color(cube.tint.y) && color(cube.tint.z) &&
           std::isfinite(norm) && std::abs(norm - 1.f) < .001f &&
           coord(cube.rotation_reference.x) && std::abs(cube.rotation_reference.x) <= 180.f &&
           coord(cube.rotation_reference.y) && std::abs(cube.rotation_reference.y) <= 180.f &&
           coord(cube.rotation_reference.z) && std::abs(cube.rotation_reference.z) <= 180.f && (cube.pattern <= 1 || cube.pattern == UINT32_MAX);
}

inline packet encode(const resource &v)
{
    packet p{'V', 'C', 'U', 'B', 1, std::uint8_t(v.type), std::uint8_t(v.enabled), 0};
    const auto &t = v.cube.pose;
    movement::detail::write_fields(p.data() + 8, v.token, v.sequence, v.index, v.revision,
        t.position.x, t.position.y, t.position.z, t.scale.x, t.scale.y, t.scale.z,
        t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
        v.cube.tint.x, v.cube.tint.y, v.cube.tint.z, v.cube.pattern, v.value,
        v.cube.rotation_reference.x, v.cube.rotation_reference.y, v.cube.rotation_reference.z);
    return p;
}

inline std::optional<resource> decode(const std::uint8_t *p, std::size_t n)
{
    if (!p || n != packet{}.size() || p[0] != 'V' || p[1] != 'C' || p[2] != 'U' || p[3] != 'B' || p[4] != 1 || p[5] < 1 || p[5] > 5 || p[6] > 1 || p[7]) return std::nullopt;
    resource v{};
    v.type = kind(p[5]);
    v.enabled = p[6] != 0;
    auto &t = v.cube.pose;
    movement::detail::read_fields(p + 8, v.token, v.sequence, v.index, v.revision,
        t.position.x, t.position.y, t.position.z, t.scale.x, t.scale.y, t.scale.z,
        t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
        v.cube.tint.x, v.cube.tint.y, v.cube.tint.z, v.cube.pattern, v.value,
        v.cube.rotation_reference.x, v.cube.rotation_reference.y, v.cube.rotation_reference.z);
    v.cube.id = v.index;
    v.cube.mesh_id = ::scene::unit_cube_mesh_id;
    if (!v.token || !v.sequence || !valid(v.cube) || !std::isfinite(v.value) ||
        (v.type == kind::create ? v.index != UINT32_MAX : !v.index || v.index == UINT32_MAX) ||
        ((v.type == kind::state || v.type == kind::ack) && !v.revision) ||
        (v.type == kind::edit && !set_property(v.cube, v.revision, v.value))) return std::nullopt;
    return v;
}

// Revisions are checked by the connection before publishing. Upsert by stable ID,
// including when a live update races the initial scene stream.
inline bool apply_state(::scene::state &world, const resource &update)
{
    const auto it = std::find_if(world.instances.begin(), world.instances.end(), [&](const auto &i) { return i.id == update.index; });
    if (!update.enabled) {
        if (it == world.instances.end()) return false;
        world.instances.erase(it);
        return true;
    }
    if (it == world.instances.end()) {
        world.instances.push_back(update.cube);
        return true;
    }
    if (::scene::same_geometry(*it, update.cube)) return false;
    *it = update.cube;
    return true;
}
} // namespace net::editor_cube
