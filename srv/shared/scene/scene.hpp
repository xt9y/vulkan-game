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
        std::vector<instance> instances;
    };
}; // namespace scene
