#pragma once
#include "util/math/quaternion.hpp"
#include "util/math/vector.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scene
{
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
    vk::util::math::vec3 angles{}; // Editor Euler degrees; rendering uses pose.rotation.
    std::uint32_t pattern = 0;
    std::uint32_t revision = 0;
    bool editor_cube = false;
    bool enabled = true;
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
};
} // namespace scene
