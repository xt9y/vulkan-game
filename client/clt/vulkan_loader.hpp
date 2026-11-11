#pragma once
#include <cstdlib>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#endif
#include <vk/vk.hpp>

namespace clt::vulkan_loader
{
struct state {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr=nullptr;
    void* library=nullptr;
    bool initialized=false;
};
state* get() {static state data;return &data;}

PFN_vkGetInstanceProcAddr init()
{
    auto& data=*get();
    if(data.initialized)return data.get_instance_proc_addr;
    data.initialized=true;
#ifdef __APPLE__
    std::vector<std::string> paths;
    const auto append=[&](const char* env,std::initializer_list<const char*> suffixes) {
        if(const auto* base=std::getenv(env))
            for(const auto* suffix:suffixes)paths.emplace_back(std::string(base)+suffix);
    };
    append("VULKAN_SDK",{"/lib/libMoltenVK.dylib","/lib/libvulkan.1.dylib"});
    append("HOMEBREW_PREFIX",{"/opt/molten-vk/lib/libMoltenVK.dylib",
                             "/lib/libMoltenVK.dylib","/lib/libvulkan.1.dylib"});
    for(const char* path:{
        "/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
        "/usr/local/opt/molten-vk/lib/libMoltenVK.dylib",
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib","libMoltenVK.dylib",
        "/opt/homebrew/lib/libvulkan.1.dylib",
        "/usr/local/lib/libvulkan.1.dylib","libvulkan.1.dylib"})
        paths.emplace_back(path);
    for(const auto& path:paths) {
        auto* library=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!library)continue;
        const auto proc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(library,"vkGetInstanceProcAddr"));
        if(proc) {
            data.library=library;
            return data.get_instance_proc_addr=proc;
        }
        dlclose(library);
    }
#endif
    return nullptr;
}

void shutdown() noexcept
{
#ifdef __APPLE__
    if(get()->library)dlclose(get()->library);
#endif
    *get()={};
}
} // namespace clt::vulkan_loader
