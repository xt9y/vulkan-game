#pragma once

#include "util/math/vector.hpp"

#include <cstddef>
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

    inline bool same_mesh(const mesh& a, const mesh& b)
    {
        if (a.id != b.id || a.vertices.size() != b.vertices.size() ||
            a.indices != b.indices)
        {
            return false;
        }

        for (std::size_t i = 0; i < a.vertices.size(); ++i)
        {
            const auto& x = a.vertices[i];
            const auto& y = b.vertices[i];
            if (x.position.x != y.position.x || x.position.y != y.position.y ||
                x.position.z != y.position.z || x.normal.x != y.normal.x ||
                x.normal.y != y.normal.y || x.normal.z != y.normal.z ||
                x.u != y.u || x.v != y.v)
            {
                return false;
            }
        }
        return true;
    }
}; // namespace scene
