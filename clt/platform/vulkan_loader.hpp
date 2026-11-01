#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace clt::vulkan_loader
{
    struct state
    {
        PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
        void* library = nullptr;
        bool initialized = false;
    };

    state* get();
    PFN_vkGetInstanceProcAddr init();
    void shutdown() noexcept;
} // namespace clt::vulkan_loader
