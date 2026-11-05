#include "renderer.hpp"
#include "srv/game/options.hpp"

#include "scene_renderer.hpp"
#include "fluid.hpp"
#include "postprocess.hpp"
#include "passes.hpp"
#include "upload.hpp"

#include "clt/platform/vulkan_loader.hpp"
#include "clt/platform/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace clt::renderer
{
    namespace
    {
        static vk::Extent2D framebuffer_extent(vk::SurfaceCapabilitiesKHR caps)
        {
            if (caps.currentExtent.width != UINT32_MAX)
            {
                return caps.currentExtent;
            }

            int width, height;
            window::framebuffer_size(width, height);

            return {
                std::clamp(
                    static_cast<uint32_t>(width),
                    caps.minImageExtent.width,
                    caps.maxImageExtent.width
                ),
                std::clamp(
                    static_cast<uint32_t>(height),
                    caps.minImageExtent.height,
                    caps.maxImageExtent.height
                )
            };
        }

    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void init(const char* vertex_path, const char* fragment_path)
    {
        if (get()->initialized)
        {
            throw std::runtime_error("clt::renderer already initialized");
        }

        if (!window::get()->handle)
        {
            throw std::runtime_error("clt::window must be initialized before the renderer");
        }

        get()->initialized = true;

        try
        {
            if (!glfwVulkanSupported())
            {
#ifdef __APPLE__
                throw std::runtime_error(
                    "Vulkan runtime not available. Install MoltenVK with: brew install molten-vk "
                    "(or install and configure the macOS Vulkan SDK)"
                );
#else
                throw std::runtime_error("Vulkan loader or driver not available");
#endif
            }
#ifdef __APPLE__
            if (!vk::loader::init(vulkan_loader::init()))
            {
                throw std::runtime_error("Vulkan loader initialization failed");
            }
#else
            if (!vk::loader::init())
            {
                throw std::runtime_error("Vulkan loader initialization failed");
            }
#endif

            uint32_t count = 0;
            const char** required = glfwGetRequiredInstanceExtensions(&count);

            if (!required || !count)
            {
                throw std::runtime_error("No Vulkan surface extensions");
            }

            std::vector<const char*> extensions(required, required + count);
            bool portability = false;

            for (const auto& extension : vk::instance::extensions())
            {
                if (std::strcmp(
                        extension.extensionName.data(),
                        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
                    ) == 0)
                {
                    portability = true;
                    break;
                }
            }

            if (portability)
            {
                extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            }

            vk::ApplicationInfo application{};
            application.pApplicationName = "vulkan-game";
            application.apiVersion = VK_API_VERSION_1_0;

            vk::InstanceCreateInfo instance_info{};
            instance_info.pApplicationInfo = &application;
            instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            instance_info.ppEnabledExtensionNames = extensions.data();

            if (portability)
            {
                instance_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
            }

            vk::instance::init(instance_info);

            VkSurfaceKHR raw_surface = VK_NULL_HANDLE;

            if (glfwCreateWindowSurface(
                    static_cast<VkInstance>(vk::instance::get()->handle),
                    window::get()->handle,
                    nullptr,
                    &raw_surface
                ) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create Vulkan surface");
            }

            vk::surface::init(vk::SurfaceKHR(raw_surface), true);

            uint32_t family = UINT32_MAX;

            for (auto gpu : vk::instance::physical_devices())
            {
                bool swapchain = false;

                for (const auto& extension : gpu.enumerateDeviceExtensionProperties())
                {
                    if (std::strcmp(
                            extension.extensionName.data(), VK_KHR_SWAPCHAIN_EXTENSION_NAME
                        ) == 0)
                    {
                        swapchain = true;
                    }
                }

                if (!swapchain)
                {
                    continue;
                }

                const auto queues = gpu.getQueueFamilyProperties();

                for (uint32_t i = 0; i < queues.size(); ++i)
                {
                    if (queues[i].queueCount &&
                        (queues[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                        (queues[i].queueFlags & vk::QueueFlagBits::eCompute) &&
                        gpu.getSurfaceSupportKHR(i, vk::surface::get()->handle))
                    {
                        vk::physical_device::adopt(gpu);
                        family = i;
                        break;
                    }
                }

                if (family != UINT32_MAX)
                {
                    break;
                }
            }

            if (family == UINT32_MAX)
            {
                throw std::runtime_error("GPU culling requires a graphics, compute and present Vulkan queue");
            }

            float priority = 1.0f;
            vk::DeviceQueueCreateInfo queue_info{};
            queue_info.queueFamilyIndex = family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;

            std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            constexpr const char* portability_subset = "VK_KHR_portability_subset";

            if (vk::physical_device::supports_extension(portability_subset))
            {
                device_extensions.push_back(portability_subset);
            }

            vk::DeviceCreateInfo device_info{};
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;
            device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
            device_info.ppEnabledExtensionNames = device_extensions.data();
            vk::device::init(device_info);
            vk::queue::init(family);
            vk::command::init(family);

            auto device = vk::device::get()->handle;
            auto commands = vk::command::allocate(2);
            for (std::size_t i = 0; i < get()->frames.size(); ++i)
            {
                auto& frame = get()->frames[i];
                frame.command = commands[i];
                frame.image_ready = device.createSemaphore({});
                frame.fence = device.createFence(
                    vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)
                );
            }
                
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
        auto& data = *get();

        auto device = vk::device::get()->handle;
        vk::device::idle();
        upload::retire(true);

        passes::release();
        fluid::release();
        postprocess::release();
        scene_renderer::release_frame_resources();
        scene_renderer::reset_scene();
        data.image_in_flight.clear();

        for (auto semaphore : data.render_ready)
        {
            device.destroySemaphore(semaphore);
        }

        data.render_ready.clear();

        const auto caps = vk::surface::capabilities();

        if (!(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eColorAttachment))
        {
            throw std::runtime_error("Surface does not support color attachments");
        }

        const auto choice = game::options::get()->graphics.surface;
        const auto requested_format =
            choice == game::options::surface_format::bgra8_srgb
                ? vk::Format::eB8G8R8A8Srgb : vk::Format::eR8G8B8A8Srgb;
        const auto formats = vk::surface::formats();
        const auto selected = std::find_if(formats.begin(), formats.end(),
            [&](const vk::SurfaceFormatKHR& value)
            {
                return value.format == requested_format &&
                    value.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
            });
        if (selected == formats.end())
            throw std::runtime_error("Configured swapchain format unsupported");
        const auto format = *selected;

        vk::SwapchainCreateInfoKHR info{};
        info.surface = vk::surface::get()->handle;
        info.minImageCount = caps.minImageCount + 1;

        if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
        {
            info.minImageCount = caps.maxImageCount;
        }

        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = framebuffer_extent(caps);
        info.imageArrayLayers = 1;
        info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        info.imageSharingMode = vk::SharingMode::eExclusive;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        const auto preferred = game::options::get()->graphics.present;
        info.presentMode = preferred == game::options::presentation::fifo
            ? vk::PresentModeKHR::eFifo
            : preferred == game::options::presentation::mailbox
                ? vk::PresentModeKHR::eMailbox
                : vk::PresentModeKHR::eImmediate;
        const auto modes = vk::surface::present_modes();
        if (std::find(modes.begin(), modes.end(), info.presentMode) == modes.end())
            throw std::runtime_error("Configured presentation mode unsupported");

        data.present_mode = info.presentMode;
        info.clipped = true;
        vk::swapchain::init(info);
        scene_renderer::create_frame_resources();
        postprocess::create();
        fluid::create();
        passes::create();

        for (uint32_t i = 0; i < vk::swapchain::get()->size; ++i)
        {
            data.render_ready.push_back(device.createSemaphore({}));
        }
        data.image_in_flight.assign(vk::swapchain::get()->size, vk::Fence{});
    }

    void draw_frame()
    {
        auto& data = *get();

        auto device = vk::device::get()->handle;
        int width, height;
        window::framebuffer_size(width, height);

        if (!width || !height)
        {
            return;
        }

        if (framebuffer_extent(vk::surface::capabilities()) !=
            vk::swapchain::get()->extent)
        {
            resize();

            return;
        }

        auto& frame = data.frames[data.next_frame];
        if (device.waitForFences(frame.fence, true, UINT64_MAX) != vk::Result::eSuccess)
        {
            throw std::runtime_error("Frame fence wait failed");
        }

        // Resource replacements are only applied once all older submissions
        // have finished. Never block the render thread waiting on every frame.
        upload::retire();
        if (scene_renderer::needs_sync())
        {
            bool safe = upload::ready();
            for (const auto& context : data.frames)
            {
                if (device.getFenceStatus(context.fence) != vk::Result::eSuccess)
                {
                    safe = false;
                }
            }
            if (!safe)
            {
                std::this_thread::yield();
                return;
            }
            scene_renderer::synchronize();
        }

        uint32_t image = 0;
        try
        {
            image = vk::swapchain::acquire(UINT64_MAX, frame.image_ready).value;
        }
        catch (const vk::OutOfDateKHRError&)
        {
            resize();
            return;
        }

        if (data.image_in_flight[image] &&
            device.waitForFences(data.image_in_flight[image], true, UINT64_MAX) !=
                vk::Result::eSuccess)
        {
            throw std::runtime_error("Swapchain image fence failed");
        }

        for (auto& pending : data.image_in_flight)
        {
            if (pending == frame.fence) pending = nullptr;
        }
        data.image_in_flight[image] = frame.fence;

        device.resetFences(frame.fence);
        frame.command.reset();
        frame.command.begin(vk::CommandBufferBeginInfo{});
        passes::record(frame.command, image);
        frame.command.end();

        vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo submit{};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.image_ready;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &data.render_ready[image];
        vk::submit::queue(vk::queue::get()->graphics, {submit}, frame.fence);

        vk::PresentInfoKHR present{};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &data.render_ready[image];
        present.swapchainCount = 1;
        present.pSwapchains = &vk::swapchain::get()->handle;
        present.pImageIndices = &image;

        data.next_frame = (data.next_frame + 1) % data.frames.size();

        try
        {
            auto result = vk::submit::present(vk::queue::get()->present, present);

            if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR)
            {
                resize();
            }
        }
        catch (const vk::OutOfDateKHRError&)
        {
            resize();
        }
    }

    void shutdown() noexcept
    {
        if (!get()->initialized)
        {
            return;
        }

        const auto device = vk::device::get()->handle;

        if (device)
        {
            try
            {
                vk::device::idle();
                upload::retire(true);
            }
            catch (...)
            {
                // Device loss must not prevent resource cleanup.
            }

            for (auto semaphore : get()->render_ready)
            {
                device.destroySemaphore(semaphore);
            }

            for (const auto& frame : get()->frames)
            {
                if (frame.fence) device.destroyFence(frame.fence);
                if (frame.image_ready) device.destroySemaphore(frame.image_ready);
            }

            passes::release();
            fluid::release();
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
        *get() = {};
    }
} // namespace clt::renderer
