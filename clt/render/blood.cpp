#include "blood.hpp"

#include "scene_renderer.hpp"
#include "texture.hpp"

#include <bld/bld.hpp>
#include <bld/math/geometry.hpp>

#include <stdexcept>
#include <vector>

namespace clt::blood
{
    namespace
    {
        struct target
        {
            vk::image::resource image{};
            vk::ImageView view{};
            bool initialized = false;
        };
        std::vector<target> targets;
    }

    void create()
    {
        if (!targets.empty())
            throw std::logic_error("Blood overlay already initialized");

        const auto extent = vk::swapchain::get()->extent;
        const auto frames = std::uint32_t(vk::swapchain::get()->images.size());
        if (!extent.width || !extent.height || !frames)
            throw std::runtime_error("Invalid blood overlay extent");

        try
        {
            bld::render::init("build/shaders/blood.comp.spv", frames,
                              extent.width, extent.height);
            targets.resize(frames);
            for (auto& target : targets)
            {
                vk::ImageCreateInfo info{};
                info.imageType = vk::ImageType::e2D;
                info.format = vk::Format::eR16G16B16A16Sfloat;
                info.extent = vk::Extent3D{extent.width, extent.height, 1};
                info.mipLevels = 1;
                info.arrayLayers = 1;
                info.samples = vk::SampleCountFlagBits::e1;
                info.tiling = vk::ImageTiling::eOptimal;
                info.usage = vk::ImageUsageFlagBits::eStorage |
                             vk::ImageUsageFlagBits::eSampled;
                target.image = vk::image::create(
                    info, vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                vk::ImageViewCreateInfo view{};
                view.image = target.image.handle;
                view.viewType = vk::ImageViewType::e2D;
                view.format = info.format;
                view.subresourceRange = {
                    vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
                };
                target.view = vk::image_view::create(view);
            }
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    vk::ImageView overlay(std::uint32_t image)
    {
        return targets.at(image).view;
    }

    void record(vk::CommandBuffer command, std::uint32_t image)
    {
        auto& target = targets.at(image);
        if (!target.initialized)
        {
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eGeneral;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = target.image.handle;
            barrier.subresourceRange = {
                vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
            };
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {}, {barrier}
            );
            target.initialized = true;
        }

        const auto gbuffer = scene_renderer::outputs(image);
        const auto projection = scene_renderer::world_view_projection();

        bld::render::input input{};
        input.width = gbuffer.extent.width;
        input.height = gbuffer.extent.height;
        input.view_projection = projection;
        input.inverse_view_projection = bld::math::inverse(projection);
        input.depth = gbuffer.views[4];
        input.receiver_ids = gbuffer.views[3];
        input.normals = gbuffer.views[1];
        input.output = target.view;
        input.sampler = texture::sampler();
        input.encoded_normals = true;
        bld::render::record(command, image, input);
    }

    void release()
    {
        for (auto& target : targets)
        {
            if (target.view) vk::image_view::destroy(target.view);
            if (target.image.handle) vk::image::destroy(target.image);
        }
        targets.clear();
        bld::render::shutdown();
    }
}
