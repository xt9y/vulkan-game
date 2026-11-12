#pragma once
#include "vulkan_loader.hpp"
#include "connection.hpp"
#include "postprocess.hpp"
#include "scene_renderer.hpp"
#include "weapon.hpp"
#include "window.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <editor/editor.hpp>
#include <editor/vulkan.hpp>
#include <gpu/graph.hpp>
#include <gpu/timestamps.hpp>
#include <gpu/upload.hpp>
#include <iostream>
#include <vector>
#include <render/frame.hpp>
#include <render/sequence.hpp>

namespace clt::renderer
{
struct state
{
    ::render::frame_scheduler frames;
    ::render::sequence passes;
    gpu::timestamps timings;
    std::array<double, 9> gpu_ms{};
    std::array<double,120> present_history{};
    std::size_t completed_samples=0;
    std::uint64_t next_submission=0;
    std::vector<std::uint64_t> submitted_ids,read_ids;
    std::uint32_t profile_frames = 0;

    bool timestamps_supported = false;
    bool initialized = false;
};

void resize();
void shutdown() noexcept;

namespace
{
static vk::Extent2D framebuffer_extent(vk::SurfaceCapabilitiesKHR caps)
{
    if (caps.currentExtent.width != UINT32_MAX)
        return caps.currentExtent;
    int width, height;
    window::framebuffer_size(width, height);

    return {
        std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width), std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height)
    };
}
} // namespace
state *get()
{
    static state data;

    return &data;
}

void init(const char *vertex_path, const char *fragment_path)
{
    if (get()->initialized)
        throw std::runtime_error("clt::renderer already initialized");

    if (!window::get()->handle)
        throw std::runtime_error("clt::window must be initialized before the renderer");
    get()->initialized = true;
    try
    {
        editor::vulkan::initialize_font();
        editor::instance().set_font_metrics(editor::vulkan::font_metrics(), 16);

        if (!glfwVulkanSupported()) {
#ifdef __APPLE__
            throw std::runtime_error("Vulkan runtime not available. Install MoltenVK with: brew install molten-vk "
                                     "(or install and configure the macOS Vulkan SDK)");
#else
            throw std::runtime_error("Vulkan loader or driver not available");
#endif
        }
#ifdef __APPLE__
        if (!vk::loader::init(vulkan_loader::init()))
            throw std::runtime_error("Vulkan loader initialization failed");
#else
        if (!vk::loader::init())
            throw std::runtime_error("Vulkan loader initialization failed");
#endif
        uint32_t count = 0;
        const char **required = glfwGetRequiredInstanceExtensions(&count);

        if (!required || !count)
            throw std::runtime_error("No Vulkan surface extensions");
        std::vector<const char *> extensions(required, required + count);

        bool portability = false;

        for (const auto &extension : vk::instance::extensions())
        {
            if (std::strcmp(extension.extensionName.data(), VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                portability = true;

                break;
            }
        }

        if (portability)
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        vk::ApplicationInfo application{};
        application.pApplicationName = "vulkan-game";
        application.apiVersion = VK_API_VERSION_1_0;
        vk::InstanceCreateInfo instance_info{};
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        instance_info.ppEnabledExtensionNames = extensions.data();

        if (portability)
            instance_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        vk::instance::init(instance_info);

        VkSurfaceKHR raw_surface = VK_NULL_HANDLE;

        if (glfwCreateWindowSurface(static_cast<VkInstance>(vk::instance::get()->handle), window::get()->handle, nullptr, &raw_surface) != VK_SUCCESS)
            throw std::runtime_error("Failed to create Vulkan surface");
        vk::surface::init(vk::SurfaceKHR(raw_surface), true);

        uint32_t family = UINT32_MAX;

        for (auto gpu : vk::instance::physical_devices())
        {
            bool swapchain = false;

            for (const auto &extension : gpu.enumerateDeviceExtensionProperties())
            {
                if (std::strcmp(extension.extensionName.data(), VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                    swapchain = true;
            }

            if (!swapchain)
                continue;

            const auto queues = gpu.getQueueFamilyProperties();

            for (uint32_t i = 0; i < queues.size(); ++i)
            {
                if (queues[i].queueCount && (queues[i].queueFlags & vk::QueueFlagBits::eGraphics) && (queues[i].queueFlags & vk::QueueFlagBits::eCompute) && gpu.getSurfaceSupportKHR(i, vk::surface::get()->handle)) {
                    vk::physical_device::adopt(gpu);
                    family = i;

                    break;
                }
            }

            if (family != UINT32_MAX)
                break;
        }

        if (family == UINT32_MAX)
            throw std::runtime_error("GPU culling requires a graphics, compute and present Vulkan queue");

        float priority = 1.0f;

        vk::DeviceQueueCreateInfo queue_info{};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        std::vector<const char *> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        constexpr const char *portability_subset = "VK_KHR_portability_subset";

        if (vk::physical_device::supports_extension(portability_subset))
            device_extensions.push_back(portability_subset);
        vk::DeviceCreateInfo device_info{};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        vk::device::init(device_info);
        vk::queue::init(family);
        vk::command::init(family);
        get()->timestamps_supported = vk::physical_device::get()->handle.getQueueFamilyProperties()[family].timestampValidBits > 0;
        get()->frames.create(1); // transport-only mapper reuses GPU photon buffers
        scene_renderer::init(vertex_path, fragment_path);
        resize();
    }

    catch (...)
    {
        shutdown();
        throw;
    }
}

void resize()
{
    auto &data = *get();
    vk::device::idle();
    data.timings.reset();
    gpu::upload::retire(true);
    data.passes.reset();
    postprocess::release();
    scene_renderer::release_frame_resources();

    const auto caps = vk::surface::capabilities();

    if (!(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eColorAttachment))
        throw std::runtime_error("Surface does not support color attachments");
    constexpr auto requested_format = vk::Format::eB8G8R8A8Srgb;

    const auto formats = vk::surface::formats();
    auto selected = std::find_if(formats.begin(), formats.end(), [&](const vk::SurfaceFormatKHR &value)
                                 { return value.format == requested_format && value.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
    if (selected == formats.end())
        selected = std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR &value)
                                { return (value.format == vk::Format::eB8G8R8A8Srgb || value.format == vk::Format::eR8G8B8A8Srgb) && value.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
    if (selected == formats.end())
        throw std::runtime_error("No supported SRGB swapchain surface format");

    const auto format = *selected;

    vk::SwapchainCreateInfoKHR info{};
    info.surface = vk::surface::get()->handle;
    info.minImageCount = caps.minImageCount + 1;

    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
        info.minImageCount = caps.maxImageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = framebuffer_extent(caps);
    info.imageArrayLayers = 1;
    info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    info.imageSharingMode = vk::SharingMode::eExclusive;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    info.presentMode = vk::PresentModeKHR::eFifo;
    info.clipped = true;
    vk::swapchain::init(info);
    scene_renderer::create_frame_resources();
    postprocess::create();
    postprocess::synchronize_scene();
    data.passes.resize(vk::swapchain::get()->images.size());
    data.frames.resize(vk::swapchain::get()->size);
    data.timings.create(static_cast<std::uint32_t>(vk::swapchain::get()->images.size()), 9, data.timestamps_supported);
    data.submitted_ids.assign(vk::swapchain::get()->images.size(),0);
    data.read_ids.assign(vk::swapchain::get()->images.size(),0);
    data.next_submission=0;
    data.completed_samples=0;
}

void draw_frame()
{
    using clock = std::chrono::steady_clock;
    static auto previous = clock::now();
    const auto now = clock::now();
    const double elapsed = std::chrono::duration<double, std::milli>(now - previous).count();
    previous = now;

    if (editor::instance().visible())
        editor::instance().set_frame_time(elapsed);
    auto &data = *get();
    int width, height;
    window::framebuffer_size(width, height);

    if (!width || !height)
        return;

    if (framebuffer_extent(vk::surface::capabilities()) != vk::swapchain::get()->extent) {
        resize();

        return;
    }

    data.frames.wait();

    for (std::uint32_t image = 0; image < vk::swapchain::get()->images.size(); ++image)
    {
        // Do not count the same completed swapchain image repeatedly.
        if(data.submitted_ids[image]==data.read_ids[image])continue;
        auto values=data.timings.read(image);
        if(values.size()==data.gpu_ms.size()) {
            std::copy(values.begin(),values.end(),data.gpu_ms.begin());
            data.present_history[data.completed_samples%data.present_history.size()]=values[5];
            ++data.completed_samples;
            data.read_ids[image]=data.submitted_ids[image];
        }
    }

    if (++data.profile_frames % 120 == 0 && data.timings.supported()) {
        const std::size_t count=std::min(data.completed_samples,
                                         data.present_history.size());
        auto sorted=data.present_history;
        std::sort(sorted.begin(),sorted.begin()+count);
        const double p50=count?sorted[(count-1)/2]:0.0;
        const double p95=count?sorted[(count-1)*95/100]:0.0;
        std::clog << "gpu(ms): geometry=" << data.gpu_ms[0]
                  << " cloud=" << data.gpu_ms[1]
                  << " gi=" << data.gpu_ms[2]
                  << " lighting=" << data.gpu_ms[3]
                  << " tonemap=" << data.gpu_ms[4]
                  << " present=" << data.gpu_ms[5]
                  << " present_p50=" << p50
                  << " present_p95=" << p95
                  // Nested timestamps in one tile-based render pass may
                  // not reflect actual fragment execution cost. Use the
                  // full presentation interval and OPTICS_PROFILE_DISABLE
                  // to measure the real difference on Metal.
                  << " optics_inner_stamp=" << data.gpu_ms[6]
                  << " mirror_light=" << data.gpu_ms[7]
                  << " mirror_tonemap=" << data.gpu_ms[8] << '\n';
    }

    gpu::upload::retire();

    if (scene_renderer::needs_sync()) {
        const bool safe = gpu::upload::ready() && data.frames.idle();

        if (!safe) {
            std::this_thread::yield();

            return;
        }

        scene_renderer::synchronize();
        postprocess::synchronize_scene();
        data.passes.invalidate();
    }
    ::render::frame_scheduler::ticket active{};
    try
    {
        active = data.frames.begin();
    }

    catch (const vk::OutOfDateKHRError &)
    {
        resize();

        return;
    }

    const auto render_camera = camera::capture();

    const pc::camera_frame gi_camera{
        render_camera.eye, render_camera.pose.yaw,
        render_camera.pose.pitch, render_camera.pose.fov
    };

    const auto target = pc::images(active.image);

    const vk::ImageSubresourceRange range{
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
    };

    data.timings.begin_frame(active.command, active.image);

    // Shared image-access declaration for the point-cloud/reflection graph.
    const auto access=[&](vk::Image image,bool writing,bool fragment=false) {
        return gpu::access::for_image(image,range,vk::ImageLayout::eGeneral,
            fragment?vk::PipelineStageFlagBits::eFragmentShader:
                     vk::PipelineStageFlagBits::eComputeShader,
            writing?vk::AccessFlagBits::eShaderWrite:
                    vk::AccessFlagBits::eShaderRead,writing);
    };
    const auto measure = [&](std::uint32_t index, gpu::graph::callback callback)
        -> gpu::graph::callback
    {
        return [timings = &data.timings, image = active.image, index, callback = std::move(callback)](vk::CommandBuffer command)
        {
            timings->start(command, image, index);
            callback(command);
            timings->end(command, image, index);
        };
    };

    const auto planar=scene_renderer::planar_reflection(render_camera);
    const bool planar_visible=planar &&
        !scene_renderer::optical_rects(render_camera).empty();
    static bool announced_planar=false;
    if(planar_visible && !announced_planar) {
        std::clog<<"optics: full-resolution planar raster reflection "
                    "(PCloud PBR + shadows + probes; no primary BVH rays)\n";
        announced_planar=true;
    }
    const auto reflected=pc::reflection_images(active.image);
    using pass=::render::sequence::pass;
    std::vector<pass> passes;
    passes.reserve(8);
    const auto add=[&](const char* name,std::uint32_t slot,
                       std::vector<gpu::access> resources,
                       gpu::graph::callback action) {
        passes.push_back({name,std::move(resources),measure(slot,std::move(action))});
    };
    const auto image=active.image;
    const auto buffer=[&](vk::Buffer handle,bool write) {
        return gpu::access::for_buffer(handle,
            vk::PipelineStageFlagBits::eComputeShader,
            write?vk::AccessFlagBits::eShaderWrite:
                  vk::AccessFlagBits::eShaderRead,write);
    };
    add("geometry",0,{},[=](vk::CommandBuffer cmd) {
        scene_renderer::draw(cmd,image,render_camera);
        if(planar_visible)
            scene_renderer::draw_reflection(cmd,image,render_camera,*planar);
        else {
            postprocess::update_skinned(image,render_camera);
            postprocess::update_photons(cmd,image,render_camera);
        }
    });
    add("point-cloud",1,{buffer(target.points,true)},
        [=](vk::CommandBuffer cmd){pc::cloud(cmd,image);});
    add("probe-grid",2,{buffer(target.points,false),access(target.grid,true)},
        [=](vk::CommandBuffer cmd){pc::gi(cmd,image);});
    add("lighting",3,{access(target.grid,false),access(target.hdr,true)},
        [=](vk::CommandBuffer cmd){pc::lighting(cmd,image,gi_camera);});
    add("tonemapping",4,{access(target.hdr,false),access(target.ldr,true)},
        [=](vk::CommandBuffer cmd){pc::tonemap(cmd,image,gi_camera);});
    add("mirror-lighting",7,{access(target.grid,false),access(reflected.hdr,true)},
        [=](vk::CommandBuffer cmd) {
            if(!planar_visible)return;
            const auto& r=planar->reflected;
            const auto& v=r.view;
            pc::reflection_lighting(cmd,image,{
                r.eye,{-v(2,0),-v(2,1),-v(2,2)},
                {v(0,0),v(0,1),v(0,2)},
                {v(1,0),v(1,1),v(1,2)},r.pose.fov});
        });
    add("mirror-tonemap",8,
        {access(reflected.hdr,false),access(reflected.ldr,true)},
        [=](vk::CommandBuffer cmd) {
            if(planar_visible)pc::reflection_tonemap(cmd,image);
        });
    add("presentation",5,{
        access(target.ldr,false,true),access(target.hdr,false,true),
        access(target.grid,false,true),access(reflected.ldr,false,true)
    },[=,timings=&data.timings](vk::CommandBuffer cmd) {
        postprocess::present(cmd,image,render_camera,timings);
    });
    data.passes.record(active.command,image,std::move(passes));

    try
    {
        auto result = data.frames.submit(active);

        data.timings.submitted(active.image);
        data.submitted_ids[active.image]=++data.next_submission;

        if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR)
            pc::updated(active.image);

        if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR)
            resize();
    }

    catch (const vk::OutOfDateKHRError &)
    {
        resize();
    }
}

void shutdown() noexcept
{
    if (!get()->initialized)
        return;

    const auto device = vk::device::get()->handle;

    if (device) {
        try
        {
            vk::device::idle();
            gpu::upload::retire(true);
        }

        catch (...)
        {
        }

        get()->frames.reset();
        get()->passes.reset();
        get()->timings.reset();
        postprocess::release();
        scene_renderer::shutdown();
        vk::command::shutdown();
        vk::swapchain::shutdown();
        vk::queue::shutdown();
        vk::device::shutdown();
    }

    vk::physical_device::shutdown();
    vk::surface::shutdown();
    vk::instance::shutdown();
    vk::loader::shutdown();
    get()->initialized = false;
}

} // namespace clt::renderer
