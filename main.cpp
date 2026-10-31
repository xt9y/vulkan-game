#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk/vk.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static vk::Extent2D framebuffer_extent(GLFWwindow *window, vk::SurfaceCapabilitiesKHR caps)
{
    if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    return {
        std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width),
        std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height)
    };
}

int main()
{
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "vulkan-game", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    try {
        if (!glfwVulkanSupported()) throw std::runtime_error("Vulkan loader not available");
        if (!vk::loader::init()) throw std::runtime_error("Vulkan loader initialization failed");

        uint32_t count = 0;
        const char **required = glfwGetRequiredInstanceExtensions(&count);
        if (!required || !count) throw std::runtime_error("No Vulkan surface extensions");

        std::vector<const char *> extensions(required, required + count);
        bool portability = false;
        for (const auto &extension : vk::instance::extensions()) {
            if (std::strcmp(extension.extensionName.data(),
                            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                portability = true;
                break;
            }
        }
        if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        vk::ApplicationInfo application{};
        application.pApplicationName = "vulkan-game";
        application.apiVersion = VK_API_VERSION_1_0;

        vk::InstanceCreateInfo instance_info{};
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        instance_info.ppEnabledExtensionNames = extensions.data();
        if (portability) instance_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        vk::instance::init(instance_info);

        VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
        if (glfwCreateWindowSurface(
                static_cast<VkInstance>(vk::instance::get()->handle),
                window, nullptr, &raw_surface) != VK_SUCCESS)
            throw std::runtime_error("Failed to create Vulkan surface");
        vk::surface::init(vk::SurfaceKHR(raw_surface), true);

        uint32_t family = UINT32_MAX;
        for (auto gpu : vk::instance::physical_devices()) {
            bool swapchain = false;
            for (const auto &extension : gpu.enumerateDeviceExtensionProperties()) {
                if (std::strcmp(extension.extensionName.data(), VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                    swapchain = true;
            }
            if (!swapchain) continue;

            const auto queues = gpu.getQueueFamilyProperties();
            for (uint32_t i = 0; i < queues.size(); ++i) {
                if (queues[i].queueCount &&
                    (queues[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                    gpu.getSurfaceSupportKHR(i, vk::surface::get()->handle)) {
                    vk::physical_device::adopt(gpu);
                    family = i;
                    break;
                }
            }
            if (family != UINT32_MAX) break;
        }
        if (family == UINT32_MAX)
            throw std::runtime_error("No graphics GPU with Vulkan presentation support");

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

        auto device = vk::device::get()->handle;
        auto commands = vk::command::allocate();
        auto command = commands.front();
        auto image_ready = device.createSemaphore({});
        auto frame_fence = device.createFence(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
        std::vector<vk::Semaphore> render_ready;

        auto resize = [&]() {
            vk::device::idle();
            for (auto semaphore : render_ready) device.destroySemaphore(semaphore);
            render_ready.clear();

            const auto caps = vk::surface::capabilities();
            if (!(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst))
                throw std::runtime_error("Surface does not support transfer-destination images");

            auto format = vk::surface::formats().front();
            for (auto candidate : vk::surface::formats()) {
                if (candidate.format == vk::Format::eB8G8R8A8Srgb &&
                    candidate.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                    format = candidate;
                    break;
                }
            }

            vk::SwapchainCreateInfoKHR info{};
            info.surface = vk::surface::get()->handle;
            info.minImageCount = caps.minImageCount + 1;
            if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
                info.minImageCount = caps.maxImageCount;
            info.imageFormat = format.format;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = framebuffer_extent(window, caps);
            info.imageArrayLayers = 1;
            info.imageUsage = vk::ImageUsageFlagBits::eTransferDst;
            info.imageSharingMode = vk::SharingMode::eExclusive;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
            info.presentMode = vk::PresentModeKHR::eFifo;
            info.clipped = true;
            vk::swapchain::init(info);

            for (uint32_t i = 0; i < vk::swapchain::get()->size; ++i)
                render_ready.push_back(device.createSemaphore({}));
        };

        resize();
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            if (!width || !height) {
                glfwWaitEvents();
                continue;
            }
            if (framebuffer_extent(window, vk::surface::capabilities()) !=
                vk::swapchain::get()->extent) {
                resize();
                continue;
            }

            if (device.waitForFences(frame_fence, true, UINT64_MAX) != vk::Result::eSuccess)
                throw std::runtime_error("Frame fence wait failed");
            uint32_t image = 0;
            try {
                image = vk::swapchain::acquire(UINT64_MAX, image_ready).value;
            } catch (const vk::OutOfDateKHRError &) {
                resize();
                continue;
            }

            device.resetFences(frame_fence);
            command.reset();
            command.begin(vk::CommandBufferBeginInfo{});

            const auto swap_image = vk::swapchain::get()->images[image];
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = swap_image;
            barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);

            const float wave = 0.5f + 0.5f * std::sin(glfwGetTime() * 0.5);
            vk::ClearColorValue clear(std::array<float, 4>{0.03f, 0.05f + 0.08f * wave, 0.10f, 1.0f});
            command.clearColorImage(
                swap_image, vk::ImageLayout::eTransferDstOptimal,
                clear, barrier.subresourceRange);

            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = {};
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, barrier);
            command.end();

            vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
            vk::SubmitInfo submit{};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &image_ready;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render_ready[image];
            vk::submit::queue(vk::queue::get()->graphics, {submit}, frame_fence);

            vk::PresentInfoKHR present{};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &render_ready[image];
            present.swapchainCount = 1;
            present.pSwapchains = &vk::swapchain::get()->handle;
            present.pImageIndices = &image;
            try {
                auto result = vk::submit::present(vk::queue::get()->present, present);
                if (result == vk::Result::eErrorOutOfDateKHR ||
                    result == vk::Result::eSuboptimalKHR) resize();
            } catch (const vk::OutOfDateKHRError &) {
                resize();
            }
        }

        vk::device::idle();
        for (auto semaphore : render_ready) device.destroySemaphore(semaphore);
        device.destroyFence(frame_fence);
        device.destroySemaphore(image_ready);
        vk::command::shutdown();
        vk::swapchain::shutdown();
        vk::queue::shutdown();
        vk::device::shutdown();
        vk::physical_device::shutdown();
        vk::surface::shutdown();
        vk::instance::shutdown();
        vk::loader::shutdown();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "vulkan-game: %s\n", error.what());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
