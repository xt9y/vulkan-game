#pragma once

#include "mesh.hpp"
#include <array>
#include <string>
#include <limits>
#include "util/math/quaternion.hpp"

namespace scene 
{
    struct transform {
        vk::util::math::vec3 position;
        vk::util::math::vec3 scale{1, 1, 1};
        vk::util::math::quat rotation;
    };

    struct clip {
        std::string name;
        std::vector<std::uint8_t> bytes;
    };

    struct skin {
        std::uint32_t id = 0;
        std::vector<std::uint8_t> skeleton;
        std::vector<clip> clips;
        std::vector<std::array<float, 16>> inverse_bind;
    };

    struct instance {
        std::uint32_t id = 0;
        std::uint32_t mesh_id = 0;
        std::uint32_t animation = 0;
        std::uint32_t animation_tick = 0;
        std::uint32_t overlay_animation = UINT32_MAX;
        std::uint32_t overlay_root = UINT32_MAX;
        float overlay_weight = 0.0f;
        transform pose;
    };

    // Game-authored world-space AABBs, usable by physics systems without
    // repeating mesh dimensions and object positions in the client.
    struct collision_box {
        vk::util::math::vec3 minimum{};
        vk::util::math::vec3 maximum{};
    };

    struct point_light
    {
        vk::util::math::vec3 position{};
        float radius=1.0f;
        vk::util::math::vec3 color{1.0f,1.0f,1.0f};
        float strength=1.0f;
    };

    struct state {
        std::vector<mesh> meshes;
        std::vector<texture> textures;
        std::vector<skin> skins;
        std::vector<instance> instances;
        std::vector<collision_box> collision_boxes;
        std::vector<point_light> lights;
    };

    inline bool same_instance(const instance& a, const instance& b)
    {
        return a.id == b.id && a.mesh_id == b.mesh_id &&
            a.animation == b.animation && a.animation_tick == b.animation_tick &&
            a.overlay_animation == b.overlay_animation &&
            a.overlay_root == b.overlay_root &&
            a.overlay_weight == b.overlay_weight &&
            a.pose.position.x == b.pose.position.x &&
            a.pose.position.y == b.pose.position.y &&
            a.pose.position.z == b.pose.position.z &&
            a.pose.scale.x == b.pose.scale.x &&
            a.pose.scale.y == b.pose.scale.y &&
            a.pose.scale.z == b.pose.scale.z &&
            a.pose.rotation.x == b.pose.rotation.x &&
            a.pose.rotation.y == b.pose.rotation.y &&
            a.pose.rotation.z == b.pose.rotation.z &&
            a.pose.rotation.w == b.pose.rotation.w;
    }
}; // namespace scene
