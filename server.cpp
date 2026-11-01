#include "srv/app/application.hpp"
#include "game/scene.hpp"
#include "game/world/gen.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "game/config.hpp"
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
        game::options::init(argc, argv, game::options::mode::server_only,
                            game::config::settings());
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        ::scene::state initial_scene;
        game::scene::create_start(initial_scene);
        srv::application::set_scene(std::move(initial_scene));

        // Game-owned asset declarations; imported meshes and animations stream to clients.
        srv::application::load_model("../MODELS/hospital_hallway.glb",
                                     ::scene::transform{}, 0);

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
