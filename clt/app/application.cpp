#include "application.hpp"

#include "clt/input/input.hpp"
#include "clt/network/connection.hpp"
#include "clt/player/prediction.hpp"
#include "clt/view/camera.hpp"
#include "clt/platform/vulkan_loader.hpp"
#include "clt/platform/window.hpp"
#include "clt/render/renderer.hpp"
#include "clt/render/postprocess.hpp"
#include "srv/game/application.hpp"

#include <atomic>
#include <chrono>
#include <exception>
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
                connection::check_error();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            connection::check_error();

            const bool connected = connection::get()->connected.load(std::memory_order_relaxed);
            const auto scene = connection::snapshot();

            return connected && scene ? 0 : 1;
        }

        window::init();

        input::init();
        camera::init();
        prediction::init();

        renderer::init("build/shaders/mesh.vert.spv", "build/shaders/mesh.frag.spv");
        bool connected = false;

        std::atomic<bool> render_stop{false};
        std::exception_ptr render_error;
        std::thread render_thread([&]
        {
            try
            {
                while (!render_stop.load(std::memory_order_relaxed))
                {
                    if (!window::minimized())
                    {
                        renderer::draw_frame();
                    }
                    else
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            }
            catch (...)
            {
                render_error = std::current_exception();
                render_stop.store(true, std::memory_order_relaxed);
            }
        });

        try
        {
            while (running() && !window::should_close() &&
                   !render_stop.load(std::memory_order_relaxed))
            {
                connection::check_error();
                window::poll_events();

                const auto frame = input::poll();
                if (frame.toggle_lighting)
                    postprocess::toggle(postprocess::effect::lighting);
                if (frame.toggle_tonemapping)
                    postprocess::toggle(postprocess::effect::tonemapping);
                camera::look(frame.mouse_x, frame.mouse_y);
                prediction::controls(frame.buttons, camera::snapshot().yaw);

                const bool current = connection::get()->connected.load(std::memory_order_relaxed);
                if (current != connected)
                {
                    window::set_connected(current);
                    connected = current;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        catch (...)
        {
            render_stop.store(true, std::memory_order_relaxed);
            render_thread.join();
            throw;
        }

        render_stop.store(true, std::memory_order_relaxed);
        render_thread.join();

        if (render_error)
        {
            std::rethrow_exception(render_error);
        }

        return 0;
    }

    void shutdown() noexcept
    {
        prediction::shutdown();
        renderer::shutdown();

        input::shutdown();
        window::shutdown();

        vulkan_loader::shutdown();
        connection::shutdown();
        get()->initialized = false;
    }
} // namespace clt::application
