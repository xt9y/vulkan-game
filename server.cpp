#include "srv/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/scene/scene.hpp"
#include "srv/shared/player/movement.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>

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

    ::scene::mesh make_floor(std::uint32_t id, float half)
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

    ::scene::mesh make_cube(std::uint32_t id, float h)
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

    void stop(int)
    {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }
}

int main(int argc, char** argv)
{
    try
    {
        game::options::state settings{};
        settings.bind_host = "127.0.0.1";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
        game::options::init(argc, argv, game::options::mode::server_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        player::configure({
            {0, 0, 4}, // spawn
            5.0f,      // walking speed
            8.0f,      // sprint speed
            20.0f,     // gravity
            0.3f,      // player radius (collision width)
            1.8f       // player height (collision and camera)
        });

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        ::scene::state initial_scene;
        auto floor = make_floor(1, 10.0f);
        auto cube = make_cube(2, 0.5f);
        initial_scene.meshes.push_back(std::move(floor));
        initial_scene.meshes.push_back(std::move(cube));

        auto add = [&](std::uint32_t id, std::uint32_t mesh, vk::util::math::vec3 position)
        {
            ::scene::instance object{};
            object.id = id;
            object.mesh_id = mesh;
            object.pose.position = position;
            initial_scene.instances.push_back(object);
        };
        add(1, 1, {0, 0, 0});
        add(2, 2, {-2, 0.5f, 0});
        add(3, 2, {0, 0.5f, 0});
        add(4, 2, {2, 0.5f, 0});
        srv::application::set_scene(std::move(initial_scene));

        // Game-owned asset declarations; imported meshes and animations stream to clients.
        ::scene::transform hallway{};
        hallway.position = {0, 0, 0};
        hallway.rotation = {0, 0, 0, 1};
        hallway.scale = {1, 1, 1};
        srv::application::load_model("../MODELS/hospital_hallway.glb", hallway, 0);

        srv::application::init();
        const int result = srv::application::run();
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-server: " << e.what() << '\n';
        return 1;
    }
}
