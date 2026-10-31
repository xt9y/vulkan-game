#include "renderer.hpp"

#include "scene_renderer.hpp"

#include "clt/platform/vulkan_loader.hpp"
#include "clt/platform/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace clt::renderer
{
    namespace
    {
        static vk::Extent2D framebuffer_extent(GLFWwindow* window, vk::SurfaceCapabilitiesKHR caps)
        {
            if (caps.currentExtent.width != UINT32_MAX)
            {
                return caps.currentExtent;
            }

            int width, height;
            glfwGetFramebufferSize(window, &width, &height);

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

    void init()
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
                throw std::runtime_error("No graphics GPU with Vulkan presentation support");
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
            auto commands = vk::command::allocate();
            get()->command = commands.front();
            get()->image_ready = device.createSemaphore({});
            get()->frame_fence =
                device.createFence(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));

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

        scene_renderer::release_frame_resources();

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

        auto format = vk::surface::formats().front();

        for (auto candidate : vk::surface::formats())
        {
            if (candidate.format == vk::Format::eB8G8R8A8Srgb &&
                candidate.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
            {
                format = candidate;
                break;
            }
        }

        vk::SwapchainCreateInfoKHR info{};
        info.surface = vk::surface::get()->handle;
        info.minImageCount = caps.minImageCount + 1;

        if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
        {
            info.minImageCount = caps.maxImageCount;
        }

        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = framebuffer_extent(window::get()->handle, caps);
        info.imageArrayLayers = 1;
        info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        info.imageSharingMode = vk::SharingMode::eExclusive;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        info.presentMode = vk::PresentModeKHR::eFifo;
        info.clipped = true;
        vk::swapchain::init(info);
        scene_renderer::create_frame_resources();

        for (uint32_t i = 0; i < vk::swapchain::get()->size; ++i)
        {
            data.render_ready.push_back(device.createSemaphore({}));
        }
    }

    void draw_frame()
    {
        auto& data = *get();

        auto device = vk::device::get()->handle;
        int width, height;
        glfwGetFramebufferSize(window::get()->handle, &width, &height);

        if (!width || !height)
        {
            return;
        }

        if (framebuffer_extent(window::get()->handle, vk::surface::capabilities()) !=
            vk::swapchain::get()->extent)
        {
            resize();

            return;
        }

        if (device.waitForFences(data.frame_fence, true, UINT64_MAX) != vk::Result::eSuccess)
        {
            throw std::runtime_error("Frame fence wait failed");
        }
        scene_renderer::synchronize();

        uint32_t image = 0;

        try
        {
            image = vk::swapchain::acquire(UINT64_MAX, data.image_ready).value;
        }
        catch (const vk::OutOfDateKHRError&)
        {
            resize();

            return;
        }

        device.resetFences(data.frame_fence);
        data.command.reset();
        data.command.begin(vk::CommandBufferBeginInfo{});
        scene_renderer::draw(data.command, image);
        data.command.end();

        vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo submit{};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &data.image_ready;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &data.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &data.render_ready[image];
        vk::submit::queue(vk::queue::get()->graphics, {submit}, data.frame_fence);

        vk::PresentInfoKHR present{};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &data.render_ready[image];
        present.swapchainCount = 1;
        present.pSwapchains = &vk::swapchain::get()->handle;
        present.pImageIndices = &image;

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
            }
            catch (...)
            {
                // Device loss must not prevent resource cleanup.
            }

            for (auto semaphore : get()->render_ready)
            {
                device.destroySemaphore(semaphore);
            }

            if (get()->frame_fence)
            {
                device.destroyFence(get()->frame_fence);
            }

            if (get()->image_ready)
            {
                device.destroySemaphore(get()->image_ready);
            }

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
