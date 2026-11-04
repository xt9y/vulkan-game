#pragma once

#include "util/math/vector.hpp"
#include "material.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace scene 
{
    struct vertex {
        vk::util::math::vec3 position;
        vk::util::math::vec3 normal;
        float u = 0;
        float v = 0;
        std::array<std::uint32_t, 4> joints{};
        std::array<float, 4> weights{};
    };

    struct mesh {
        std::uint32_t id = 0;
        std::uint32_t skin_id = 0;
        bool bleeds = false;
        std::vector<vertex> vertices;
        std::vector<std::uint32_t> indices;
        material surface;
    };

    inline bool same_mesh(const mesh& a, const mesh& b)
    {
        if (a.id != b.id || a.skin_id != b.skin_id || a.bleeds != b.bleeds ||
            a.vertices.size() != b.vertices.size() ||
            a.indices != b.indices || !same_material(a.surface, b.surface))
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
                x.u != y.u || x.v != y.v || x.joints != y.joints ||
                x.weights != y.weights)
            {
                return false;
            }
        }
        return true;
    }
}; // namespace scene
