#pragma once

#include "mesh.hpp"
#include "util/math/quaternion.hpp"

namespace scene 
{
    struct transform {
        vk::util::math::vec3 position;
        vk::util::math::vec3 scale{1, 1, 1};
        vk::util::math::quat rotation;
    };

    struct instance {
        std::uint32_t id = 0;
        std::uint32_t mesh_id = 0;
        transform pose;
    };

    struct state {
        std::vector<mesh> meshes;
        std::vector<texture> textures;
        std::vector<instance> instances;
    };

    inline bool same_instance(const instance& a, const instance& b)
    {
        return a.id == b.id && a.mesh_id == b.mesh_id &&
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
