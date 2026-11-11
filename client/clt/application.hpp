#pragma once
#include "camera.hpp"
#include "connection.hpp"
#include "input.hpp"
#include "prediction.hpp"
#include "renderer.hpp"
#include "weapon.hpp"
#include "window.hpp"
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <editor/editor.hpp>
#include <iostream>
#include <stdexcept>

namespace clt::application
{
inline constexpr int restart_code = 80;

struct state
{
    bool initialized = false;
};

namespace
{
std::atomic<bool> typed_question{false};
void editor_character(GLFWwindow *, unsigned int codepoint)
{
    if (codepoint == static_cast<unsigned int>('?'))
        typed_question.store(true, std::memory_order_relaxed);
}
} // namespace
state *get()
{
    static state data;

    return &data;
}

void init()
{
    if (get()->initialized)
        throw std::runtime_error("clt::application already initialized");
    connection::get()->host = game::options::get()->host;
    connection::get()->port = game::options::get()->port;
    connection::init();
    get()->initialized = true;
}

int run()
{
    if (!get()->initialized)
        throw std::runtime_error("clt::application is not initialized");

    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    auto running = [&]
    {
        if (game::application::get()->stop.load(std::memory_order_relaxed))
            return false;

        if (game::options::get()->duration == 0)
            return true;

        const auto elapsed = std::chrono::duration<double>(clock::now() - started);

        return elapsed.count() < game::options::get()->duration;
    };

    if (game::options::get()->headless) {
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
    glfwSetCharCallback(window::get()->handle, editor_character);
    input::init();
    camera::init();
    prediction::init();
    pc::set_exposure(game::options::get()->graphics.exposure);
    renderer::init(game::options::get()->graphics.vertex_shader.c_str(), game::options::get()->graphics.fragment_shader.c_str());
    bool restart_requested=false;
    std::atomic<bool> render_stop{false};
    std::exception_ptr render_error;
    std::thread render_thread([&]
                              {
            try {
                while (!render_stop.load(std::memory_order_relaxed)) {
                    if (!window::minimized())
                        renderer::draw_frame();
                    else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            } catch (...) {
                render_error = std::current_exception();
                render_stop.store(true, std::memory_order_relaxed);
            } });
    try
    {
        while (running() && !window::should_close() && !render_stop.load(std::memory_order_relaxed))
        {
            connection::check_error();
            window::poll_events();

            const auto frame = input::poll();

            static std::array<bool, 10> held{};
            static std::array<clock::time_point,4> repeat_at{};

            const auto pressed = [](int key, std::size_t index)
            {
                const bool down = input::key_down(key);
                const bool edge = down && !held[index];
                held[index] = down;

                return edge;
            };

            const auto repeating = [&](int key,std::size_t index) {
                const bool down=input::key_down(key);
                const bool edge=pressed(key,index);
                auto& next=repeat_at[index-4];
                if(!down) {next={};return false;}
                const auto now=clock::now();
                if(edge) {
                    next=now+std::chrono::milliseconds(250);
                    return true;
                }
                if(now>=next) {
                    next=now+std::chrono::milliseconds(40);
                    return true;
                }
                return false;
            };
            if(pressed(GLFW_KEY_0,8) || pressed(GLFW_KEY_KP_0,9)) {
                restart_requested=true;
                break;
            }

            editor::input events{};
            events.question = typed_question.exchange(false, std::memory_order_relaxed);
            events.tab = pressed(GLFW_KEY_TAB, 0);

            const bool main_enter = pressed(GLFW_KEY_ENTER, 1);
            const bool keypad_enter = pressed(GLFW_KEY_KP_ENTER, 2);
            events.enter = main_enter || keypad_enter;
            events.escape = pressed(GLFW_KEY_ESCAPE, 3);
            events.up = repeating(GLFW_KEY_UP, 4);
            events.down = repeating(GLFW_KEY_DOWN, 5);
            events.left = repeating(GLFW_KEY_LEFT, 6);
            events.right = repeating(GLFW_KEY_RIGHT, 7);
            events.shift = input::key_down(GLFW_KEY_LEFT_SHIFT) || input::key_down(GLFW_KEY_RIGHT_SHIFT);
            events.command = input::key_down(GLFW_KEY_LEFT_SUPER) ||
                             input::key_down(GLFW_KEY_RIGHT_SUPER);

            static bool mouse_held = false;
            const bool mouse_down = input::mouse_down(GLFW_MOUSE_BUTTON_LEFT);
            events.click = mouse_down && !mouse_held;
            events.mouse_down = mouse_down;
            mouse_held = mouse_down;

            auto *handle = window::get()->handle;
            double cursor_x = 0, cursor_y = 0;
            int window_width = 0, window_height = 0;
            int display_width = 0, display_height = 0;
            glfwGetCursorPos(handle, &cursor_x, &cursor_y);
            glfwGetWindowSize(handle, &window_width, &window_height);
            window::framebuffer_size(display_width, display_height);
            events.viewport_width = display_width;
            events.viewport_height = display_height;
            events.mouse_x = int(cursor_x * double(display_width) / double(std::max(1, window_width)));
            events.mouse_y = int(cursor_y * double(display_height) / double(std::max(1, window_height)));
            editor::instance().handle(events);

            if (events.tab || events.question) {
                const bool free_cursor = editor::instance().cursor_free();
                glfwSetInputMode(handle, GLFW_CURSOR, free_cursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);

                if (glfwRawMouseMotionSupported())
                    glfwSetInputMode(handle, GLFW_RAW_MOUSE_MOTION, free_cursor ? GLFW_FALSE : GLFW_TRUE);
            }

            const bool captured = editor::instance().captures_input();

            // Properties capture gameplay keys, but camera look only depends
            // on whether Tab has released the cursor for pointing at the UI.
            if (!editor::instance().cursor_free() && !events.tab)
                camera::look(frame.mouse_x, frame.mouse_y);
            prediction::controls(captured ? 0u : frame.buttons, camera::snapshot().yaw);
            on_input(frame);

            for (const auto &action : editor::instance().take_actions())
                on_editor_action(action);
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
        std::rethrow_exception(render_error);

    return restart_requested?restart_code:0;
}

void shutdown() noexcept
{
    prediction::shutdown();
    renderer::shutdown();
    editor::instance().reset();
    input::shutdown();
    window::shutdown();
    vulkan_loader::shutdown();
    connection::shutdown();
    get()->initialized = false;
}

} // namespace clt::application
