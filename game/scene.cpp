#include "scene.hpp"
#include "srv/shared/scene/scene.hpp"
#include "util/math/vector.hpp"

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

        ::scene::mesh floor()
        {
            ::scene::mesh mesh;
            mesh.id = 1;

            quad(
                mesh,
                {-10, 0, -10},
                {-10, 0, 10},
                {10, 0, 10},
                {10, 0, -10},
                {0, 1, 0}
            );

            return mesh;
        }

        ::scene::mesh cube()
        {
            ::scene::mesh mesh;
            mesh.id = 2;

            constexpr float h = 0.5f;

            quad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
            quad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
            quad(mesh, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0});
            quad(mesh, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0});
            quad(mesh, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0});
            quad(mesh, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0});

            return mesh;
        }
    } // namespace
    
    void create_start(::scene::state &data)
    {
        data = {};

        data.meshes.push_back(floor());
        data.meshes.push_back(cube());

        auto spawn = [&](std::uint32_t id, std::uint32_t mesh_id, vk::util::math::vec3 position)
        {
            ::scene::instance instance;
            instance.id = id;
            instance.mesh_id = mesh_id;
            instance.pose.position = position;

            data.instances.push_back(instance);
        };
        
        spawn(1, 1, {0, 0, 0});
        spawn(2, 2, {-2, 0.5f, 0});
        spawn(3, 2, {0, 0.5f, 0});
        spawn(4, 2, {2, 0.5f, 0});
    }
}; // namespace game::scene
