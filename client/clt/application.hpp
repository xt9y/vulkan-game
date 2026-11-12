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
#include <algorithm>
#include <array>
#include <exception>
#include <thread>
#include <chrono>
#include <editor/editor.hpp>
#include <iostream>
#include <stdexcept>

namespace clt::application
{
struct state {bool initialized=false;};
namespace {
std::atomic<bool> typed_question{false};
void editor_character(GLFWwindow*,unsigned int codepoint)
{
    if(codepoint==static_cast<unsigned int>('?'))
        typed_question.store(true,std::memory_order_relaxed);
}
}
state* get() {static state data;return &data;}
void init()
{
    if(get()->initialized)
        throw std::runtime_error("clt::application already initialized");
    auto& connection=*clt::connection::get();
    connection.host=game::options::get()->host;
    connection.port=game::options::get()->port;
    clt::connection::init();
    get()->initialized=true;
}
int run()
{
    if(!get()->initialized)
        throw std::runtime_error("clt::application is not initialized");
    using clock=std::chrono::steady_clock;
    const auto started=clock::now();
    const auto running=[&]() {
        return !game::application::get()->stop.load(std::memory_order_relaxed) &&
            (!game::options::get()->duration ||
             std::chrono::duration<double>(clock::now()-started).count() <
             game::options::get()->duration);
    };
    if(game::options::get()->headless) {
        while(running()) {
            connection::check_error();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        connection::check_error();
        return connection::get()->connected.load(std::memory_order_relaxed) &&
               connection::snapshot()?0:1;
    }

    window::init();
    glfwSetCharCallback(window::get()->handle,editor_character);
    input::init();
    camera::init();
    prediction::init();
    pc::set_exposure(game::options::get()->graphics.exposure);
    renderer::init(game::options::get()->graphics.vertex_shader.c_str(),
                   game::options::get()->graphics.fragment_shader.c_str());
    std::atomic<bool> render_stop{false};
    std::exception_ptr render_error;
    std::thread render_thread([&]() {
        try {
            while(!render_stop.load(std::memory_order_relaxed))
                if(!window::minimized())renderer::draw_frame();
                else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch(...) {
            render_error=std::current_exception();
            render_stop.store(true,std::memory_order_relaxed);
        }
    });

    try {
        static std::array<bool,10> held{};
        static std::array<clock::time_point,4> repeat_at{};
        static bool primed=false;
        if(!primed) {
            // Held reset keys at launch are not new presses.
            held[8]=input::key_down(GLFW_KEY_0);
            held[9]=input::key_down(GLFW_KEY_KP_0);
            primed=true;
        }
        const auto pressed=[](int key,std::size_t index) {
            const bool down=input::key_down(key);
            const bool edge=down && !held[index];
            held[index]=down;
            return edge;
        };
        const auto repeating=[&](int key,std::size_t index) {
            const bool down=input::key_down(key);
            const bool edge=pressed(key,index);
            auto& next=repeat_at[index-4];
            if(!down){next={};return false;}
            const auto now=clock::now();
            if(edge){next=now+std::chrono::milliseconds(250);return true;}
            if(now>=next){next=now+std::chrono::milliseconds(40);return true;}
            return false;
        };

        while(running() && !window::should_close() &&
              !render_stop.load(std::memory_order_relaxed)) {
            connection::check_error();
            window::poll_events();
            const auto frame=input::poll();
            const bool zero=pressed(GLFW_KEY_0,8);
            const bool keypad_zero=pressed(GLFW_KEY_KP_0,9);
            if(zero || keypad_zero) {
                camera::reset_view();
                prediction::request_reset();
            }
            editor::input events{};
            events.question=typed_question.exchange(false,std::memory_order_relaxed);
            events.tab=pressed(GLFW_KEY_TAB,0);
            const bool enter=pressed(GLFW_KEY_ENTER,1);
            const bool keypad_enter=pressed(GLFW_KEY_KP_ENTER,2);
            events.enter=enter || keypad_enter;
            events.escape=pressed(GLFW_KEY_ESCAPE,3);
            events.up=repeating(GLFW_KEY_UP,4);
            events.down=repeating(GLFW_KEY_DOWN,5);
            events.left=repeating(GLFW_KEY_LEFT,6);
            events.right=repeating(GLFW_KEY_RIGHT,7);
            events.shift=input::key_down(GLFW_KEY_LEFT_SHIFT) ||
                         input::key_down(GLFW_KEY_RIGHT_SHIFT);
            events.command=input::key_down(GLFW_KEY_LEFT_SUPER) ||
                           input::key_down(GLFW_KEY_RIGHT_SUPER);
            static bool mouse_held=false;
            const bool mouse_down=input::mouse_down(GLFW_MOUSE_BUTTON_LEFT);
            events.click=mouse_down && !mouse_held;
            events.mouse_down=mouse_down;
            mouse_held=mouse_down;

            auto* handle=window::get()->handle;
            double cursor_x=0,cursor_y=0;
            int window_width=0,window_height=0,display_width=0,display_height=0;
            glfwGetCursorPos(handle,&cursor_x,&cursor_y);
            glfwGetWindowSize(handle,&window_width,&window_height);
            window::framebuffer_size(display_width,display_height);
            events.viewport_width=display_width;
            events.viewport_height=display_height;
            events.mouse_x=int(cursor_x*double(display_width)/
                               double(std::max(1,window_width)));
            events.mouse_y=int(cursor_y*double(display_height)/
                               double(std::max(1,window_height)));
            editor::instance().handle(events);
            if(events.tab || events.question) {
                const bool free_cursor=editor::instance().cursor_free();
                glfwSetInputMode(handle,GLFW_CURSOR,
                    free_cursor?GLFW_CURSOR_NORMAL:GLFW_CURSOR_DISABLED);
                if(glfwRawMouseMotionSupported())
                    glfwSetInputMode(handle,GLFW_RAW_MOUSE_MOTION,
                        free_cursor?GLFW_FALSE:GLFW_TRUE);
            }
            const bool captured=editor::instance().captures_input();
            if(!editor::instance().cursor_free() && !events.tab)
                camera::look(frame.mouse_x,frame.mouse_y);
            prediction::controls(captured?0u:frame.buttons,camera::snapshot().yaw);
            on_input(frame);
            for(const auto& action:editor::instance().take_actions())
                on_editor_action(action);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch(...) {
        render_stop.store(true,std::memory_order_relaxed);
        render_thread.join();
        throw;
    }
    render_stop.store(true,std::memory_order_relaxed);
    render_thread.join();
    if(render_error)std::rethrow_exception(render_error);
    return 0;
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
    get()->initialized=false;
}
} // namespace clt::application
