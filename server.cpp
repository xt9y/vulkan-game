#include "srv/app/application.hpp"
#include "game/scene.hpp"
#include "game/world/gen.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>

namespace
{
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
        settings.player.spawn = {0, 0, 4};
        settings.player.walk_speed = 5.0f;
        settings.player.sprint_speed = 8.0f;
        settings.player.gravity = 20.0f;
        settings.player.radius = 0.3f;
        settings.player.height = 1.8f;
        settings.simulation.tick_hz = 60;
        settings.simulation.max_clients = 64;
        settings.simulation.timeout_seconds = 5;
        settings.simulation.receive_packets = 96;
        settings.simulation.receive_ms = 2;
        settings.simulation.fragments_per_tick = 64;
        settings.simulation.fragments_ms = 2;
        settings.simulation.inputs_per_tick = 4;
        settings.simulation.animation_period = 6;
        settings.simulation.animations_per_tick = 64;
        settings.simulation.generation_slack_ms = 2;
        settings.simulation.generation_commit_ticks = 8;
        settings.simulation.world_seed = 123456789ULL;
        settings.simulation.terrain_base = 16;
        settings.simulation.terrain_scale = 64;
        settings.simulation.terrain_amplitude = 10;
        settings.simulation.detail_scale = 16;
        settings.simulation.detail_amplitude = 3;
        game::options::init(argc, argv, game::options::mode::server_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        game::scene::configuration level{};
        level.floor_mesh = 1;
        level.cube_mesh = 2;
        level.floor_half_extent = 10.0f;
        level.cube_half_extent = 0.5f;
        auto add = [&](std::uint32_t id, std::uint32_t mesh,
                       vk::util::math::vec3 position)
        {
            ::scene::instance item{};
            item.id = id;
            item.mesh_id = mesh;
            item.pose.position = position;
            level.instances.push_back(item);
        };
        add(1, level.floor_mesh, {0, 0, 0});
        add(2, level.cube_mesh, {-2, 0.5f, 0});
        add(3, level.cube_mesh, {0, 0.5f, 0});
        add(4, level.cube_mesh, {2, 0.5f, 0});

        ::scene::state initial_scene;
        game::scene::create_start(initial_scene, level);
        srv::application::set_scene(std::move(initial_scene));

        // Game-owned asset declarations; imported meshes and animations stream to clients.
        ::scene::transform hallway{};
        hallway.position = {0, 0, 0};
        hallway.rotation = {0, 0, 0, 1};
        hallway.scale = {1, 1, 1};
        srv::application::load_model("../MODELS/hospital_hallway.glb", hallway, 0);

        srv::application::init();
        srv::world::init(srv::application::get()->terrain,
                         game::options::get()->simulation.world_seed,
                         game::world::generator::height);
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
