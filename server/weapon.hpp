#pragma once
#include "net/message.hpp"
#include "scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <span>
#include <spatial/index.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vk/vk.hpp>

namespace weapon
{
using vk::util::math::vec3;
enum class action : std::uint32_t
{
    fire = 1,
    reload = 2,
    equip = 3
};

enum class reload_phase : std::uint32_t
{
    none,
    tactical,
    empty
};

enum class result : std::uint32_t
{
    none,
    fired,
    cooldown,
    empty,
    reloading,
    reload_started,
    reload_unnecessary,
    equipped
};

struct timing
{
    float reload_duration = 0;
    float empty_reload_duration = 0;
    float refill_time = 0;
    float empty_refill_time = 0;
};

struct definition
{
    std::uint32_t id = 0;
    std::uint32_t magazine_capacity = 0;
    std::uint32_t initial_reserve = 0;

    double fire_interval = 0;
    float range = 0;
    timing reload;
};

struct state
{
    std::uint32_t magazine = 0;
    std::uint32_t reserve = 0;
    std::uint32_t sequence = 0;

    reload_phase reload = reload_phase::none;
    result last_result = result::none;
    double next_fire = 0;
    double reload_started = 0;
    double reload_finishes = 0;
    double refill_at = 0;
    bool refilled = false;
};

struct triangle
{
    vec3 a, b, c;
    std::uint32_t instance = 0, index = 0;
};

struct hit
{
    bool impacted = false;

    vec3 position{}, normal{};
    std::uint32_t instance = 0, triangle = 0;
};

struct world
{
    std::vector<triangle> triangles;
    spatial::index index;
};

enum class motion
{
    idle,
    walk,
    draw,
    reload,
    reload_empty,
    ads_idle,
    ads_in,
    ads_out,
    ads_walk,
    fire1,
    fire2,
    fire3,
    ads_fire1,
    ads_fire2,
    ads_fire3,
    count
};

struct clip
{
    std::string name;

    float speed = 1.f;
    bool loop = false;
};

struct bindings
{
    int trigger_button = 0, aim_button = 1;
    int reload_key = 'R', inspect_key = 'M';
    bool automatic = false;
    float sensitivity = .0025f;
    float ads_sensitivity_multiplier = .60f;
};

struct gun
{
    definition rules;

    bindings input{};
    std::filesystem::path model, casing_model;
    std::array<clip, std::size_t(motion::count)> animations{};
    std::string camera_joint, muzzle_joint, ejection_joint, slide_joint;
    vk::util::math::vec3 muzzle_offset{0.f, 0.f, -.4f};
    vk::util::math::vec3 ejection_offset{.08f, -.04f, -.12f};
    vk::util::math::mat4 correction = vk::util::math::mat4::identity();

    float fov = 1.2f, ads_fov = 1.2f, near_plane = .01f, far_plane = 20.f;
    float ads_zoom = 0.f, ads_speed = 14.f;
    float slide_offset = 0.f;
    float recoil = 0.f, hip_kick = 0.f, ads_kick = 0.f;
    float recoil_decay = 15.f;

    vk::util::math::vec3 casing_impulse{1.f, 1.f, 0.f};

    float casing_lifetime = 9.f, casing_spin = 14.f;

    std::uint32_t max_casings = 48;

    float flash_duration = .035f;

    vk::util::math::vec3 flash_scale{.035f, .045f, .035f};

    float impact_radius = .009f, impact_offset = .0008f, impact_lifetime = 120.f;

    std::uint32_t max_impacts = 192;
};

inline void validate(const definition &);
inline state make_state(const definition &);
inline void advance(state &, double, const definition &);
inline result apply(state &, std::uint32_t, action, double, const definition &);
inline vec3 direction(float, float);
inline void append(world &, const std::vector<scene::mesh> &, std::span<const scene::instance>);
inline hit trace(const world &, vec3, vec3, float);
inline timing load_timing(const std::filesystem::path &, const std::string &, const std::string &, float, float);
inline void validate(const definition &value)
{
    const auto &t = value.reload;

    if (!value.id || !value.magazine_capacity || !std::isfinite(value.fire_interval) || value.fire_interval <= 0 || !std::isfinite(value.range) || value.range <= 0 ||
        !std::isfinite(t.reload_duration) || !std::isfinite(t.empty_reload_duration) || !std::isfinite(t.refill_time) || !std::isfinite(t.empty_refill_time) ||
        t.reload_duration <= 0 || t.empty_reload_duration <= 0 || t.refill_time < 0 || t.refill_time > t.reload_duration || t.empty_refill_time < 0 || t.empty_refill_time > t.empty_reload_duration)
    {
        throw std::invalid_argument("Invalid weapon definition");
    }
}

inline state make_state(const definition &settings)
{
    state value;
    value.magazine = settings.magazine_capacity;
    value.reserve = settings.initial_reserve;

    return value;
}

inline void advance(state &value, double now, const definition &settings)
{
    if (value.reload == reload_phase::none)
        return;

    if (!value.refilled && now >= value.refill_at) {
        const auto amount = std::min(settings.magazine_capacity - std::min(value.magazine, settings.magazine_capacity), value.reserve);
        value.magazine += amount;
        value.reserve -= amount;
        value.refilled = true;
    }

    if (now >= value.reload_finishes)
        value.reload = reload_phase::none;
}

inline result apply(state &value, std::uint32_t sequence, action command, double now, const definition &settings)
{
    advance(value, now, settings);

    if (!sequence || !net::message::newer(sequence, value.sequence))
        return result::none;
    value.sequence = sequence;

    if (command == action::equip)
        return value.last_result = result::equipped;

    if (command != action::fire && command != action::reload)
        return value.last_result = result::none;

    if (value.reload != reload_phase::none)
        return value.last_result = result::reloading;

    if (command == action::reload) {
        if (value.magazine == settings.magazine_capacity || !value.reserve)
            return value.last_result = result::reload_unnecessary;
        const auto &reload = settings.reload;

        const bool empty = !value.magazine;
        value.reload = empty ? reload_phase::empty : reload_phase::tactical;
        value.reload_started = now;
        value.reload_finishes = now + (empty ? reload.empty_reload_duration : reload.reload_duration);
        value.refill_at = now + (empty ? reload.empty_refill_time : reload.refill_time);
        value.refilled = false;

        return value.last_result = result::reload_started;
    }

    if (now < value.next_fire)
        return value.last_result = result::cooldown;

    if (!value.magazine)
        return value.last_result = result::empty;
    --value.magazine;
    value.next_fire = now + settings.fire_interval;

    return value.last_result = result::fired;
}

inline vec3 direction(float yaw, float pitch)
{
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

inline void append(world &data, const std::vector<scene::mesh> &meshes, std::span<const scene::instance> instances)
{
    using namespace vk::util::math;
    std::vector<spatial::bounds> boxes;

    for (const auto &instance : instances)
    {
        if(!instance.enabled)continue;
        const auto mesh = std::find_if(meshes.begin(), meshes.end(), [&](const scene::mesh &m)
                                       { return m.id == instance.mesh_id; });
        if (mesh == meshes.end()) continue;

        const auto transform = [&](vec3 p)
        {
            return instance.pose.position + rotate(instance.pose.rotation,
                                                   {p.x * instance.pose.scale.x, p.y * instance.pose.scale.y, p.z * instance.pose.scale.z});
        };

        for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
        {
            const auto a = mesh->indices[i], b = mesh->indices[i + 1], c = mesh->indices[i + 2];

            if (a >= mesh->vertices.size() || b >= mesh->vertices.size() || c >= mesh->vertices.size()) continue;
            triangle t{transform(mesh->vertices[a].position), transform(mesh->vertices[b].position), transform(mesh->vertices[c].position), instance.id, std::uint32_t(i / 3)};
            data.triangles.push_back(t);
            boxes.push_back({{std::min({t.a.x, t.b.x, t.c.x}), std::min({t.a.y, t.b.y, t.c.y}), std::min({t.a.z, t.b.z, t.c.z})}, {std::max({t.a.x, t.b.x, t.c.x}), std::max({t.a.y, t.b.y, t.c.y}), std::max({t.a.z, t.b.z, t.c.z})}});
        }
    }

    data.index.append(boxes);
}

inline hit trace(const world &data, vec3 origin, vec3 direction, float range)
{
    using namespace vk::util::math;
    hit nearest{};

    float distance = range;
    const auto end = origin + direction * range;
    const spatial::bounds region{{std::min(origin.x, end.x), std::min(origin.y, end.y), std::min(origin.z, end.z)}, {std::max(origin.x, end.x), std::max(origin.y, end.y), std::max(origin.z, end.z)}};
    data.index.query(region, [&](std::uint32_t id)
                     {
            const auto& t=data.triangles[id];
            const auto edge1=t.b-t.a,edge2=t.c-t.a;
            const auto h=cross(direction,edge2);
            const float determinant=dot(edge1,h);
            if(std::abs(determinant)<1e-7f)return;
            const float inverse=1.f/determinant;
            const auto s=origin-t.a;
            const float u=inverse*dot(s,h);
            if(u<0||u>1)return;
            const auto q=cross(s,edge1);
            const float v=inverse*dot(direction,q);
            if(v<0||u+v>1)return;
            const float d=inverse*dot(edge2,q);
            if(d<.0001f||d>=distance)return;
            distance=d;
            auto normal=normalize(cross(edge1,edge2));
            if(dot(normal,direction)>0)normal=normal*-1.f;
            nearest={true,origin+direction*d,normal,t.instance,t.index}; });
    return nearest;
}

inline timing load_timing(const std::filesystem::path &path, const std::string &reload_clip, const std::string &empty_reload_clip, float refill_frame, float empty_refill_frame)
{
    std::ifstream input(path);

    if (!input)
        throw std::runtime_error("Weapon animation metadata missing: " + path.string());
    const std::string json((std::istreambuf_iterator<char>(input)), {});

    auto clip = [&](const std::string &name, float event_frame)
    {
        std::string escaped;

        for (const char c : name)
        {
            if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos)
                escaped += '\\';
            escaped += c;
        }

        const std::regex pattern("\\{[^{}]*\"name\"\\s*:\\s*\"" + escaped + "\"[^{}]*\\}");
        std::smatch match;

        if (!std::regex_search(json, match, pattern))
            throw std::runtime_error("Weapon animation metadata missing clip " + name);
        const std::string object = match.str();

        auto number = [&](const std::string &key)
        {
            const std::regex field("\"" + key + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)");
            std::smatch value;

            if (!std::regex_search(object, value, field))
                throw std::runtime_error("Weapon animation metadata missing " + key);

            return std::stof(value[1].str());
        };

        const float duration = number("duration_seconds");
        const float fps = number("fps");

        if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(duration) || duration <= 0 || !std::isfinite(event_frame))
            throw std::runtime_error("Invalid weapon animation timing");

        return std::pair{
            duration, std::max(0.0f, event_frame - number("source_first_frame")) / fps
        };
    };

    const auto normal = clip(reload_clip, refill_frame), empty = clip(empty_reload_clip, empty_refill_frame);

    return {normal.first, empty.first, normal.second, empty.second};
}

} // namespace weapon
