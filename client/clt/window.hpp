#pragma once
#include "vulkan_loader.hpp"
#include <GLFW/glfw3.h>
#include <atomic>
#include <stdexcept>

namespace clt::window
{
struct state {
    GLFWwindow* handle=nullptr;
    bool initialized=false;
};
state* get() {static state data;return &data;}

namespace {
std::atomic<int> framebuffer_width{},framebuffer_height{};
void update_framebuffer_size()
{
    int width=0,height=0;
    glfwGetFramebufferSize(get()->handle,&width,&height);
    framebuffer_width.store(width,std::memory_order_relaxed);
    framebuffer_height.store(height,std::memory_order_relaxed);
}
}
void shutdown() noexcept;

void init()
{
    auto& data=*get();
    if(data.initialized)throw std::runtime_error("clt::window already initialized");
#ifdef __APPLE__
    if(const auto loader=vulkan_loader::init())glfwInitVulkanLoader(loader);
#endif
    if(!glfwInit())throw std::runtime_error("GLFW initialization failed");
    data.initialized=true;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    const auto& graphics=game::options::get()->graphics;
    data.handle=glfwCreateWindow(graphics.width,graphics.height,
                                 graphics.title.c_str(),nullptr,nullptr);
    if(!data.handle) {
        shutdown();
        throw std::runtime_error("Window creation failed");
    }
    update_framebuffer_size();
}

bool should_close() {return glfwWindowShouldClose(get()->handle);}
void framebuffer_size(int& width,int& height)
{
    width=framebuffer_width.load(std::memory_order_relaxed);
    height=framebuffer_height.load(std::memory_order_relaxed);
}
bool minimized()
{
    int width,height;
    framebuffer_size(width,height);
    return !width || !height;
}
void poll_events() {glfwPollEvents();update_framebuffer_size();}
void shutdown() noexcept
{
    if(!get()->initialized)return;
    if(get()->handle)glfwDestroyWindow(get()->handle);
    glfwTerminate();
    *get()={};
}
} // namespace clt::window
