#pragma once
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
#include <render/frame.hpp>
#include <render/sequence.hpp>

namespace clt::renderer
{
struct state
{
    ::render::frame_scheduler frames;
    ::render::sequence passes;
    gpu::timestamps timings;
    std::array<double, 7> gpu_ms{};
    std::uint32_t profile_frames = 0;

    bool timestamps_supported = false;
    bool initialized = false;
};

state *get();
void init(const char *vertex_path, const char *fragment_path);
void resize();
void draw_frame();
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
    data.timings.create(static_cast<std::uint32_t>(vk::swapchain::get()->images.size()), 7, data.timestamps_supported);
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
        auto values = data.timings.read(image);

        if (values.size() == data.gpu_ms.size())
            std::copy(values.begin(), values.end(), data.gpu_ms.begin());
    }

    if (++data.profile_frames % 120 == 0 && data.timings.supported()) {
        std::clog << "gpu(ms): geometry=" << data.gpu_ms[0]
                  << " cloud=" << data.gpu_ms[1]
                  << " gi=" << data.gpu_ms[2]
                  << " lighting=" << data.gpu_ms[3]
                  << " tonemap=" << data.gpu_ms[4]
                  << " present=" << data.gpu_ms[5]
                  << " optics=" << data.gpu_ms[6] << '\n';
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

    data.passes.record(active.command, active.image, {{"geometry", {}, measure(0, [image = active.image, render_camera](vk::CommandBuffer cmd)
                                                                               { scene_renderer::draw(cmd, image, render_camera);
                                                                                 postprocess::update_photons(cmd,image,render_camera); })},
                                                      {"point-cloud", {gpu::access::for_buffer(target.points, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderWrite, true)}, measure(1, [image = active.image](vk::CommandBuffer cmd)
                                                                                                                                                                                                           { pc::cloud(cmd, image); })},
                                                      {"probe-grid", {gpu::access::for_buffer(target.points, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead, false), gpu::access::for_image(target.grid, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderWrite, true)}, measure(2, [image = active.image](vk::CommandBuffer cmd)
                                                                                                                                                                                                                                                                                                                                                                    { pc::gi(cmd, image); })},
                                                      {"lighting", {gpu::access::for_image(target.grid, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead, false), gpu::access::for_image(target.hdr, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderWrite, true)}, measure(3, [image = active.image, gi_camera](vk::CommandBuffer cmd)
                                                                                                                                                                                                                                                                                                                                                                                                { pc::lighting(cmd, image, gi_camera); })},
                                                      {"tonemapping", {gpu::access::for_image(target.hdr, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead, false), gpu::access::for_image(target.ldr, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderWrite, true)}, measure(4, [image = active.image, gi_camera](vk::CommandBuffer cmd)
                                                                                                                                                                                                                                                                                                                                                                                                  { pc::tonemap(cmd, image, gi_camera); })},
                                                      {"presentation", {gpu::access::for_image(target.ldr, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead, false),
                                                                        gpu::access::for_image(target.hdr, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead, false),
                                                                        gpu::access::for_image(target.grid, range, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead, false)}, measure(5, [image = active.image, render_camera, timings = &data.timings](vk::CommandBuffer cmd)
                                                                                                                                                                                                                                           { postprocess::present(cmd, image, render_camera, timings); })}});
    try
    {
        auto result = data.frames.submit(active);

        data.timings.submitted(active.image);

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
