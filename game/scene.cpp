#include "scene.hpp"
#include "srv/shared/scene/scene.hpp"
#include "util/math/vector.hpp"
#include <stdexcept>

namespace game::scene
{
    namespace 
    {
        void quad(::scene::mesh& mesh, vk::util::math::vec3 a, vk::util::math::vec3 b, vk::util::math::vec3 c, vk::util::math::vec3 d, vk::util::math::vec3 normal)
        {
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());

            mesh.vertices.push_back({a, normal, 0, 0});
            mesh.vertices.push_back({b, normal, 1, 0});
            mesh.vertices.push_back({c, normal, 1, 1});
            mesh.vertices.push_back({d, normal, 0, 1});

            mesh.indices.insert(mesh.indices.end(), {
                base, base + 1, base + 2,
                base, base + 2, base + 3,
            });
        }

        ::scene::mesh floor(std::uint32_t id, float half)
        {
            ::scene::mesh mesh;
            mesh.id = id;

            quad(
                mesh,
                {-half, 0, -half},
                {-half, 0, half},
                {half, 0, half},
                {half, 0, -half},
                {0, 1, 0}
            );

            return mesh;
        }

        ::scene::mesh cube(std::uint32_t id, float h)
        {
            ::scene::mesh mesh;
            mesh.id = id;

            quad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
            quad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
            quad(mesh, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0});
            quad(mesh, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0});
            quad(mesh, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0});
            quad(mesh, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0});

            return mesh;
        }
    } // namespace
    
    void create_start(::scene::state& data, const configuration& config)
    {
        if (config.floor_mesh == config.cube_mesh ||
            config.floor_half_extent <= 0 || config.cube_half_extent <= 0)
            throw std::invalid_argument("Invalid game scene configuration");

        data = {};
        data.meshes.push_back(floor(config.floor_mesh, config.floor_half_extent));
        data.meshes.push_back(cube(config.cube_mesh, config.cube_half_extent));
        data.instances = config.instances;
    }
} // namespace game::scene
