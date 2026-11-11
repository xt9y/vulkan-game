#pragma once
#include "util/math/quaternion.hpp"
#include "util/math/vector.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>

namespace scene
{
// The level reserves mesh 2 for shared editable unit-cube geometry.
inline constexpr std::uint32_t ground_mesh_id = 1;
inline constexpr std::uint32_t unit_cube_mesh_id = 2;
inline constexpr std::uint32_t unit_sphere_mesh_id = 3;
inline constexpr std::uint32_t mermaid_insert_mesh_id = UINT32_MAX - 1;
struct texture
{
    std::uint32_t id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> encoded; // Original PNG/JPEG bytes for compact network transfer.
};

struct material
{
    std::array<float, 4> base_color{1, 1, 1, 1};
    std::array<float, 3> emissive{0, 0, 0};

    float metallic = 0;
    float roughness = 1;
    float occlusion = 1;
    float alpha_cutoff = 0.5f;

    std::uint32_t base_color_texture = 0;
    std::uint32_t metallic_roughness_texture = 0;
    std::uint32_t normal_texture = 0;
    std::uint32_t occlusion_texture = 0;
    std::uint32_t emissive_texture = 0;
    std::uint32_t alpha_mode = 0; // opaque, mask, blend
    bool double_sided = false;

    std::uint32_t procedural_pattern = 0;
};

struct vertex
{
    vk::util::math::vec3 position;
    vk::util::math::vec3 normal;

    float u = 0;
    float v = 0;

    std::array<std::uint32_t, 4> joints{};
    std::array<float, 4> weights{};
};

struct mesh
{
    std::uint32_t id = 0;
    std::uint32_t skin_id = 0;
    std::vector<vertex> vertices;
    std::vector<std::uint32_t> indices;

    material surface;
};

struct transform
{
    vk::util::math::vec3 position;
    vk::util::math::vec3 scale{1, 1, 1};
    vk::util::math::quat rotation;
};

// Shared model transform for physics, rays, GI and optical bounds.
inline vk::util::math::vec3 transform_position(const transform& pose,
                                               vk::util::math::vec3 point)
{
    return pose.position+vk::util::math::rotate(pose.rotation,{
        point.x*pose.scale.x,point.y*pose.scale.y,point.z*pose.scale.z});
}

struct clip
{
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct skin
{
    std::uint32_t id = 0;
    std::vector<std::uint8_t> skeleton;
    std::vector<clip> clips;
    std::vector<std::array<float, 16>> inverse_bind;
};

struct instance
{
    std::uint32_t id = 0;
    std::uint32_t mesh_id = 0;
    std::uint32_t animation = 0;
    std::uint32_t animation_tick = 0;
    std::uint32_t overlay_animation = UINT32_MAX;
    std::uint32_t overlay_root = UINT32_MAX;

    float overlay_weight = 0.0f;
    transform pose;
    vk::util::math::vec3 tint{1, 1, 1};
    std::uint32_t pattern = UINT32_MAX; // 0 solid, 1 checker, 2 glass, 3 reflective; UINT32_MAX inherits.
    std::uint32_t editor_group = 0; // Shared root instance ID for multi-mesh Mermaid models.
    float optical_strength = .9f; // transmission or reflectivity
    float optical_mattness = .05f; // [0,1]
    // Editor reference selects continuous Euler controls at/through the poles.
    // Rendering and simulation always consume pose.rotation.
    vk::util::math::vec3 rotation_reference{};
};

struct collision_box
{
    vk::util::math::vec3 minimum{};
    vk::util::math::vec3 maximum{};
};

struct point_light
{
    vk::util::math::vec3 position{};

    float radius = 1.0f;

    vk::util::math::vec3 color{1.0f, 1.0f, 1.0f};

    float strength = 1.0f;
    bool enabled = true;
};

struct state
{
    std::vector<mesh> meshes;
    std::vector<texture> textures;
    std::vector<skin> skins;
    std::vector<instance> instances;
    std::vector<collision_box> collision_boxes;
    std::vector<point_light> lights;
    // Client-local immutable asset generation. Deliberately not serialized:
    // increments only when scene mesh/texture payloads arrive.
    std::uint64_t atlas_revision=0;
};
inline bool same_pose(const instance &a, const instance &b)
{
    const auto fields = [](const instance &i) {
        const auto &p = i.pose;
        return std::array{p.position.x, p.position.y, p.position.z, p.scale.x, p.scale.y, p.scale.z,
                          p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w};
    };
    return a.id == b.id && a.mesh_id == b.mesh_id && fields(a) == fields(b);
}

inline bool same_geometry(const instance &a, const instance &b)
{
    return same_pose(a, b) && a.tint.x == b.tint.x && a.tint.y == b.tint.y && a.tint.z == b.tint.z && a.pattern == b.pattern &&
           a.optical_strength == b.optical_strength && a.optical_mattness == b.optical_mattness && a.editor_group == b.editor_group &&
           a.rotation_reference.x == b.rotation_reference.x && a.rotation_reference.y == b.rotation_reference.y &&
           a.rotation_reference.z == b.rotation_reference.z;
}

inline bool geometry_prefix_unchanged(const state &previous, const state &incoming, bool surface = false)
{
    if (previous.meshes.size() != incoming.meshes.size() || previous.instances.size() > incoming.instances.size()) return false;
    for (std::size_t i = 0; i < previous.instances.size(); ++i)
        if (surface ? !same_geometry(previous.instances[i], incoming.instances[i]) : !same_pose(previous.instances[i], incoming.instances[i])) return false;
    return true;
}
// Mesh IDs are shared across every instance; resolve them in one scan.
template<class Visit>
void each_mesh_instance(const std::vector<mesh>& meshes,
                        std::span<const instance> instances,Visit&& visit)
{
    std::unordered_map<std::uint32_t,const mesh*> by_id;
    by_id.reserve(meshes.size());
    for(const auto& m:meshes)by_id.emplace(m.id,&m);
    for(const auto& instance:instances)
        if(const auto found=by_id.find(instance.mesh_id);found!=by_id.end())
            visit(*found->second,instance);
}
} // namespace scene
