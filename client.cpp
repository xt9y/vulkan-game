#include "clt/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "game/config.hpp"
#include "srv/shared/net/socket.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <utility>
#include <GLFW/glfw3.h>

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
        auto settings = game::config::settings();
        settings.controls = {
            GLFW_KEY_W, GLFW_KEY_S, GLFW_KEY_A, GLFW_KEY_D,
            GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT,
            GLFW_KEY_TAB, GLFW_KEY_ESCAPE, GLFW_KEY_F6, GLFW_KEY_F7,
            true, GLFW_KEY_F1, GLFW_KEY_UP, GLFW_KEY_DOWN,
            GLFW_KEY_LEFT, GLFW_KEY_RIGHT
        };
        game::options::init(argc, argv, game::options::mode::client_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        clt::application::init();
        const int result = clt::application::run();
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-client: " << e.what() << '\n';
        return 1;
    }
}
