#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/world/world.hpp"

#include <csignal>
#include <exception>
#include <iostream>

namespace
{
    static_assert(
        std::atomic<bool>::is_always_lock_free, "Signal shutdown requires lock-free atomics"
    );

    void stop(int)
    {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        game::options::init(argc, argv);

        if (game::options::get()->help)
        {
            game::options::print_help();
            game::options::shutdown();
            return 0;
        }

        game::application::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop); 

        const int result = game::application::run();

        game::application::shutdown();
        game::options::shutdown();

        return result;
    }
    catch (const std::exception& error)
    {
        game::application::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-game: " << error.what() << '\n';

        return 1;
    }
}
