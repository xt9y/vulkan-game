#pragma once

#include "util/math/vector.hpp"

#include <cstdint>
#include <vector>

namespace scene 
{
    struct vertex {
        vk::util::math::vec3 position;
        vk::util::math::vec3 normal;
        float u = 0;
        float v = 0;
    };

    struct mesh {
        std::uint32_t id = 0;
        std::vector<vertex> vertices;
        std::vector<std::uint32_t> indices;
    };
}; // namespace scene
