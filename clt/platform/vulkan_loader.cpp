#include "vulkan_loader.hpp"
#ifdef __APPLE__
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace
{
    // GLFW 3.4 cannot find Homebrew's or SDK's Vulkan dylib by its standard name.
    // Keep the selected library loaded for the duration of the process.
    PFN_vkGetInstanceProcAddr discover_loader()
    {
        std::vector<std::string> paths;

        if (const char* sdk = std::getenv("VULKAN_SDK"))
        {
            paths.emplace_back(std::string(sdk) + "/lib/libMoltenVK.dylib");
            paths.emplace_back(std::string(sdk) + "/lib/libvulkan.1.dylib");
        }

        if (const char* prefix = std::getenv("HOMEBREW_PREFIX"))
        {
            paths.emplace_back(std::string(prefix) + "/opt/molten-vk/lib/libMoltenVK.dylib");
            paths.emplace_back(std::string(prefix) + "/lib/libMoltenVK.dylib");
            paths.emplace_back(std::string(prefix) + "/lib/libvulkan.1.dylib");
        }

        for (const char* path :
             {"/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
              "/usr/local/opt/molten-vk/lib/libMoltenVK.dylib",
              "/opt/homebrew/lib/libMoltenVK.dylib",
              "/usr/local/lib/libMoltenVK.dylib",
              "libMoltenVK.dylib",
              "/opt/homebrew/lib/libvulkan.1.dylib",
              "/usr/local/lib/libvulkan.1.dylib",
              "libvulkan.1.dylib"})
        {
            paths.emplace_back(path);
        }

        for (const auto& path : paths)
        {
            void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

            if (!library)
            {
                continue;
            }

            auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(library, "vkGetInstanceProcAddr")
            );

            if (proc)
            {
                clt::vulkan_loader::get()->library = library;

                return proc;
            }

            dlclose(library);
        }

        return nullptr;
    }
} // namespace
#endif

namespace clt::vulkan_loader
{
    state* get()
    {
        static state data;
        return &data;
    }

    PFN_vkGetInstanceProcAddr init()
    {
        if (!get()->initialized)
        {
#ifdef __APPLE__
            get()->get_instance_proc_addr = discover_loader();
#endif
            get()->initialized = true;
        }

        return get()->get_instance_proc_addr;
    }

    void shutdown() noexcept
    {
#ifdef __APPLE__
        if (get()->library)
        {
            dlclose(get()->library);
        }
#endif
        *get() = {};
    }
} // namespace clt::vulkan_loader
