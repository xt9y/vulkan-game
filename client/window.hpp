#pragma once
struct GLFWwindow;

namespace clt::window {
    struct state {
        GLFWwindow* handle = nullptr;
        int width = 0;
        int height = 0;
        bool initialized = false;
    };
    state* get();
    void init();
    bool should_close();
    bool minimized();
    void framebuffer_size(int& width, int& height);
    void poll_events();
    void shutdown() noexcept;

} // namespace clt::window
