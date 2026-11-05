#include "srv/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/player/movement.hpp"
#include "srv/shared/scene/scene.hpp"
#include "srv/shared/weapon/metadata.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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

    ::scene::mesh make_rect(std::uint32_t id, float x, float y, float z)
    {
        ::scene::mesh mesh;
        mesh.id = id;
    
        quad(mesh, {-x, -y,  z}, { x, -y,  z}, { x,  y,  z}, {-x,  y,  z}, { 0,  0,  1});
        quad(mesh, { x, -y, -z}, {-x, -y, -z}, {-x,  y, -z}, { x,  y, -z}, { 0,  0, -1});
        quad(mesh, {-x, -y, -z}, {-x, -y,  z}, {-x,  y,  z}, {-x,  y, -z}, {-1,  0,  0});
        quad(mesh, { x, -y,  z}, { x, -y, -z}, { x,  y, -z}, { x,  y,  z}, { 1,  0,  0});
        quad(mesh, {-x,  y,  z}, { x,  y,  z}, { x,  y, -z}, {-x,  y, -z}, { 0,  1,  0});
        quad(mesh, {-x, -y, -z}, { x, -y, -z}, { x, -y,  z}, {-x, -y,  z}, { 0, -1,  0});
    
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
            4.0f,      // walking speed
            14.0f,     // sprint speed
            20.0f,     // gravity
            0.3f,      // player radius (collision width)
            1.8f       // player height (collision and camera)
        });

        // Weapon IDs, ammunition, fire cadence and animation events belong to this game.
        weapon::definition glock;
        glock.id = 1;
        glock.magazine_capacity = 15;
        glock.initial_reserve = 90;
        glock.fire_interval = .15;
        glock.range = 200;
        glock.reload = weapon::load_timing(
            "assets/glock/metadata.json", "Glock_Reload", "Glock_ReloadEmpty", 20, 20
        );
        srv::application::add_weapon(glock);
        srv::application::on_weapon_command(
            [](srv::application::weapon_command_context& event)
            {
                const auto origin = event.player.position;
                const auto direction = weapon::direction(event.command.yaw, event.command.pitch);

                const auto world_hit = event.world.trace(
                    origin, direction, event.definition.range
                );
                
                const auto distance = [&](const weapon::hit& h) {
                    return h.impacted
                        ? vk::util::math::length_squared(h.position - origin)
                        : std::numeric_limits<float>::infinity();
                };
                
                return world_hit;
            }
        );

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        ::scene::state initial_scene;
        // make_rect takes half extents. Keep the walking surface at y = 0
        // and join four solid walls around the edge of the floor, open above.
        auto floor = make_rect(1, 10.0f, 0.1f, 10.0f);
        auto cube = make_rect(2, 0.5f, 0.5f, 0.5f);
        auto rect1 = make_rect(3, 0.34f, 2.0f, 5.0f);
        auto rect2 = make_rect(4, 5.0f, 2.0f, 0.34f);
        initial_scene.meshes.push_back(std::move(floor));
        initial_scene.meshes.push_back(std::move(cube));
        initial_scene.meshes.push_back(std::move(rect1));
        initial_scene.meshes.push_back(std::move(rect2));

        auto add = [&](std::uint32_t id, std::uint32_t mesh, vk::util::math::vec3 position)
        {
            ::scene::instance object{};
            object.id = id;
            object.mesh_id = mesh;
            object.pose.position = position;
            initial_scene.instances.push_back(object);
        };
        add(1, 1, {0, -0.1f, 0});
        add(2, 2, {-2.f, .5f, 0});
        add(3, 2, {0, .5f, 0});
        add(4, 2, {2.f, .5f, 0});
        add(5, 3, {5.0f, 2.0f, 0});
        add(6, 3, {-5.0f, 2.0f, 0});
        add(7, 4, {0, 2.0f, 5.0f});
        add(8, 4, {0, 2.0f, -5.0f});
        // Restore the original seated mermaid above the middle cube.
        const auto& center_cube = initial_scene.instances.at(2);
        ::scene::transform mermaid = center_cube.pose;
        mermaid.position.y += 0.5f;
        mermaid.position.z += 0.5f / 3.f;
        mermaid.scale = {1.5f, 1.5f, 1.5f};
        srv::application::load_model("assets/mermaid/mermaid.glb", mermaid, 0);

        // Water is simulated client-side and collides with the same boxes.
        srv::application::set_scene(std::move(initial_scene));

        // Game-owned asset declarations; imported meshes and animations stream to clients.
        // ::scene::transform hallway{};
        // hallway.position = {0, 0, 0};
        // hallway.rotation = {0, 0, 0, 1};
        // hallway.scale = {1, 1, 1};
        // srv::application::load_model("../MODELS/hospital_hallway.glb", hallway, 0);

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
