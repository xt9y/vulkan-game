#pragma once
#include "vulkan_loader.hpp"
#include <GLFW/glfw3.h>
#include <atomic>
#include <stdexcept>
struct GLFWwindow;

namespace clt::window
{
struct state
{
    GLFWwindow *handle = nullptr;
    int width = 0;
    int height = 0;
    bool initialized = false;
};

state *get();
void init();
bool should_close();
bool minimized();
void framebuffer_size(int &width, int &height);
void poll_events();
void shutdown() noexcept;

namespace
{
std::atomic<int> framebuffer_width{};
std::atomic<int> framebuffer_height{};
void update_framebuffer_size()
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(get()->handle, &width, &height);
    framebuffer_width.store(width, std::memory_order_relaxed);
    framebuffer_height.store(height, std::memory_order_relaxed);
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
        throw std::runtime_error("clt::window already initialized");
#ifdef __APPLE__
    if (const auto loader = vulkan_loader::init())
        glfwInitVulkanLoader(loader);
#endif
    if (!glfwInit())
        throw std::runtime_error("GLFW initialization failed");
    get()->initialized = true;
    get()->width = game::options::get()->graphics.width;
    get()->height = game::options::get()->graphics.height;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    get()->handle = glfwCreateWindow(get()->width, get()->height,
                                     game::options::get()->graphics.title.c_str(), nullptr, nullptr);
    if (!get()->handle) {
        shutdown();
        throw std::runtime_error("Window creation failed");
    }

    update_framebuffer_size();
}

bool should_close()
{
    return glfwWindowShouldClose(get()->handle);
}

void framebuffer_size(int &width, int &height)
{
    width = framebuffer_width.load(std::memory_order_relaxed);
    height = framebuffer_height.load(std::memory_order_relaxed);
}

bool minimized()
{
    int width, height;
    framebuffer_size(width, height);

    return width == 0 || height == 0;
}

void poll_events()
{
    glfwPollEvents();
    update_framebuffer_size();
}

void shutdown() noexcept
{
    if (!get()->initialized)
        return;

    if (get()->handle)
        glfwDestroyWindow(get()->handle);
    glfwTerminate();
    *get() = {};
}

} // namespace clt::window
