#include "window.hpp"

#include "vulkan_loader.hpp"

#include <stdexcept>

namespace clt::window
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
            throw std::runtime_error("clt::window already initialized");
        }

#ifdef __APPLE__
        if (const auto loader = vulkan_loader::init())
        {
            glfwInitVulkanLoader(loader);
        }
#endif

        if (!glfwInit())
        {
            throw std::runtime_error("GLFW initialization failed");
        }

        get()->initialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        get()->handle =
            glfwCreateWindow(get()->width, get()->height, get()->title.c_str(), nullptr, nullptr);

        if (!get()->handle)
        {
            shutdown();
            throw std::runtime_error("Window creation failed");
        }
    }

    bool should_close()
    {
        return glfwWindowShouldClose(get()->handle);
    }

    bool minimized()
    {
        int width, height;
        glfwGetFramebufferSize(get()->handle, &width, &height);

        return width == 0 || height == 0;
    }

    void poll_events()
    {
        glfwPollEvents();
    }

    void set_connected(bool connected)
    {
        get()->title = connected ? "vulkan-game | connected" : "vulkan-game | connecting";
        glfwSetWindowTitle(get()->handle, get()->title.c_str());
    }

    void shutdown() noexcept
    {
        if (!get()->initialized)
        {
            return;
        }

        if (get()->handle)
        {
            glfwDestroyWindow(get()->handle);
        }

        glfwTerminate();
        *get() = {};
    }
} // namespace clt::window
