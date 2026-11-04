#include "passes.hpp"
#include "postprocess.hpp"
#include "scene_renderer.hpp"
#include "blood.hpp"

#include <stdexcept>
#include <vector>
#include <array>

namespace clt::passes
{
    namespace
    {
        struct state {
            std::vector<bool> initialized;
        };

        state& get()
        {
            static state value;
            return value;
        }

        void transition(vk::CommandBuffer command, vk::Image image)
        {
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eGeneral;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {
                vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
            };
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {}, {barrier}
            );
        }

        void dependency(vk::CommandBuffer command,
                        vk::PipelineStageFlags src,
                        vk::PipelineStageFlags dst,
                        vk::AccessFlags src_access,
                        vk::AccessFlags dst_access)
        {
            vk::MemoryBarrier barrier{};
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;
            command.pipelineBarrier(src, dst, {}, {barrier}, {}, {});
        }
    }

    const std::array<description, 5>& ordered()
    {
        static const std::array<description, 5> list{{
            {"geometry", vk::PipelineBindPoint::eGraphics, resource::scene,
                resource::gbuffer, scene_renderer::draw},
            {"lighting", vk::PipelineBindPoint::eCompute, resource::gbuffer,
                resource::hdr, postprocess::lighting},
            {"fluid", vk::PipelineBindPoint::eCompute,
                resource::gbuffer | resource::hdr, resource::liquid, blood::record},
            {"tonemapping", vk::PipelineBindPoint::eCompute,
                resource::hdr | resource::liquid, resource::ldr,
                postprocess::tonemap},
            {"presentation", vk::PipelineBindPoint::eGraphics, resource::ldr,
                resource::swapchain, postprocess::present}
        }};
        return list;
    }

    void create()
    {
        get().initialized.assign(vk::swapchain::get()->images.size(), false);
    }

    void release()
    {
        get().initialized.clear();
    }

    void step(vk::CommandBuffer command, std::uint32_t image, std::uint32_t index)
    {
        auto& data = get();
        const auto& list = ordered();
        if (index >= list.size())
        {
            throw std::runtime_error("Render pass index out of range");
        }

        if (index == 1)
        {
            if (!data.initialized.at(image))
            {
                const auto target = postprocess::images(image);
                transition(command, target.hdr);
                transition(command, target.ldr);
                data.initialized[image] = true;
            }
            // Geometry supplies its external attachment-to-compute dependency.
        }
        else if (index == 2 || index == 3)
        {
            dependency(command,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite,
                vk::AccessFlagBits::eShaderRead);
        }
        else if (index == 4)
        {
            dependency(command,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eFragmentShader,
                vk::AccessFlagBits::eShaderWrite,
                vk::AccessFlagBits::eShaderRead);
        }
        list[index].record(command, image);
    }

    void record(vk::CommandBuffer command, std::uint32_t image)
    {
        std::uint32_t available = resource::scene;
        const auto& list = ordered();
        for (std::uint32_t i = 0; i < list.size(); ++i)
        {
            if ((list[i].reads & available) != list[i].reads)
            {
                throw std::runtime_error("Render pass has an unsatisfied resource dependency");
            }
            step(command, image, i);
            available |= list[i].writes;
        }
    }
}
