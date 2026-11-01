#include "srv/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>

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
        game::options::init(argc, argv, game::options::mode::server_only);
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
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
