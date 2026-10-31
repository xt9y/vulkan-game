#pragma once

#include <string>

struct GLFWwindow;

namespace clt::window
{
    struct state
    {
        GLFWwindow* handle = nullptr;
        int width = 1280;
        int height = 720;
        std::string title = "vulkan-game | connecting";
        bool initialized = false;
    };

    state* get();
    void init();
    bool should_close();
    bool minimized();
    void poll_events();
    void set_connected(bool connected);
    void shutdown() noexcept;
} // namespace clt::window
