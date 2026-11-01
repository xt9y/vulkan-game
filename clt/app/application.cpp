#include "application.hpp"

#include "clt/network/connection.hpp"
#include "clt/platform/vulkan_loader.hpp"
#include "clt/platform/window.hpp"
#include "clt/render/renderer.hpp"
#include "srv/game/application.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace clt::application
{
    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        if (get()->initialized)
        {
            throw std::runtime_error("clt::application already initialized");
        }

        get()->options = *game::options::get();

        if (get()->options.mode == game::options::mode::server_and_client)
        {
            get()->options.host =
                get()->options.bind_host == "0.0.0.0" ? "127.0.0.1" : get()->options.bind_host;
        }

        connection::get()->host = get()->options.host;
        connection::get()->port = get()->options.port;
        connection::init();
        get()->initialized = true;
    }

    int run()
    {
        if (!get()->initialized)
        {
            throw std::runtime_error("clt::application is not initialized");
        }

        using clock = std::chrono::steady_clock;
        const auto started = clock::now();

        auto running = [&]
        {
            if (game::application::get()->stop.load(std::memory_order_relaxed))
            {
                return false;
            }

            if (get()->options.duration == 0)
            {
                return true;
            }

            const auto elapsed = std::chrono::duration<double>(clock::now() - started);

            return elapsed.count() < get()->options.duration;
        };

        if (get()->options.headless)
        {
            while (running())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            const bool connected = connection::get()->connected.load(std::memory_order_relaxed);
            std::cout << (connected ? "connected" : "disconnected") << ", server tick: "
                      << connection::get()->server_tick.load(std::memory_order_relaxed) << '\n';

            return connected ? 0 : 1;
        }

        window::init();
        renderer::init();
        bool connected = false;

        while (running() && !window::should_close())
        {
            window::poll_events();
            const bool current = connection::get()->connected.load(std::memory_order_relaxed);

            if (current != connected)
            {
                window::set_connected(current);
                connected = current;
            }

            if (window::minimized())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            else
            {
                renderer::draw_frame();
            }
        }

        return 0;
    }

    void shutdown() noexcept
    {
        renderer::shutdown();
        window::shutdown();
        vulkan_loader::shutdown();
        connection::shutdown();
        get()->initialized = false;
    }
} // namespace clt::application
