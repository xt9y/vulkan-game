#include "renderer.hpp"
#include "srv/game/options.hpp"

#include "scene_renderer.hpp"
#include "postprocess.hpp"
#include <gpu/graph.hpp>
#include <gpu/upload.hpp>

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

            get()->frames.create(2);
                
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


        vk::device::idle();
        gpu::upload::retire(true);

        data.passes.reset();
        postprocess::release();
        scene_renderer::release_frame_resources();
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

        info.clipped = true;
        vk::swapchain::init(info);
        scene_renderer::create_frame_resources();
        postprocess::create();
        data.passes.resize(vk::swapchain::get()->images.size());

        data.frames.resize(vk::swapchain::get()->size);
    }

    void draw_frame()
    {
        auto& data = *get();


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

        data.frames.wait();

        // Resource replacements are only applied once all older submissions
        // have finished. Never block the render thread waiting on every frame.
        gpu::upload::retire();
        if (scene_renderer::needs_sync())
        {
            const bool safe = gpu::upload::ready() && data.frames.idle();
            if (!safe)
            {
                std::this_thread::yield();
                return;
            }
            scene_renderer::synchronize();
        }

        ::render::frame_scheduler::ticket active{};
        try
        {
            active = data.frames.begin();
        }
        catch (const vk::OutOfDateKHRError&)
        {
            resize();
            return;
        }

        // Geometry retains its render-pass external dependency into compute.
        // All subsequent image hazards are tracked by the reusable GPU graph.
        const auto target=postprocess::images(active.image);
        const vk::ImageSubresourceRange range{
            vk::ImageAspectFlagBits::eColor,0,1,0,1
        };
        data.passes.record(active.command,active.image,{
            {"geometry",{},[image=active.image](vk::CommandBuffer cmd)
                {scene_renderer::draw(cmd,image);}},
            {"lighting",{gpu::access::for_image(target.hdr,range,
                vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite,true)},
                [image=active.image](vk::CommandBuffer cmd)
                {postprocess::lighting(cmd,image);}},
            {"tonemapping",{
                gpu::access::for_image(target.hdr,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead,false),
                gpu::access::for_image(target.ldr,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,true)},
                [image=active.image](vk::CommandBuffer cmd)
                {postprocess::tonemap(cmd,image);}},
            {"presentation",{gpu::access::for_image(target.ldr,range,
                vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits::eFragmentShader,
                vk::AccessFlagBits::eShaderRead,false)},
                [image=active.image](vk::CommandBuffer cmd)
                {postprocess::present(cmd,image);}}
        });

        try
        {
            auto result = data.frames.submit(active);

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
                gpu::upload::retire(true);
            }
            catch (...)
            {
                // Device loss must not prevent resource cleanup.
            }

            get()->frames.reset();
            get()->passes.reset();
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
