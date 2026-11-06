#include "passes.hpp"
#include "postprocess.hpp"
#include "scene_renderer.hpp"

#include <gpu/graph.hpp>
#include <stdexcept>
#include <vector>
#include <array>

namespace clt::passes
{
    namespace
    {
        struct state {
            // Per-swapchain-image state survives frames, not swapchain recreation.
            std::vector<gpu::graph> graphs;
        };

        state& get()
        {
            static state value;
            return value;
        }

        vk::ImageSubresourceRange color_range()
        {
            return {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        }
    }

    const std::array<description, 4>& ordered()
    {
        static const std::array<description, 4> list{{
            {"geometry", vk::PipelineBindPoint::eGraphics, resource::scene,
                resource::gbuffer, scene_renderer::draw},
            {"lighting", vk::PipelineBindPoint::eCompute, resource::gbuffer,
                resource::hdr, postprocess::lighting},
            {"tonemapping", vk::PipelineBindPoint::eCompute,
                resource::hdr, resource::ldr, postprocess::tonemap},
            {"presentation", vk::PipelineBindPoint::eGraphics, resource::ldr,
                resource::swapchain, postprocess::present}
        }};
        return list;
    }

    void create()
    {
        auto& graphs = get().graphs;
        graphs.clear();
        graphs.resize(vk::swapchain::get()->images.size());
    }

    void release()
    {
        get().graphs.clear();
    }

    void step(vk::CommandBuffer command, std::uint32_t image, std::uint32_t index)
    {
        const auto& list = ordered();
        if (index >= list.size())
            throw std::runtime_error("Render pass index out of range");

        if (index == 0)
        {
            // The geometry render pass retains its attachment-to-compute
            // external subpass dependency for G-buffer sampling.
            list[index].record(command, image);
            return;
        }

        auto& graph = get().graphs.at(image);
        graph.clear_passes();
        const auto target = postprocess::images(image);
        const auto range = color_range();

        if (index == 1)
        {
            graph.add("lighting",
                {gpu::access::for_image(target.hdr, range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite, true)},
                [image](vk::CommandBuffer cmd) { postprocess::lighting(cmd, image); });
        }
        else if (index == 2)
        {
            graph.add("tonemapping",
                {gpu::access::for_image(target.hdr, range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead, false),
                 gpu::access::for_image(target.ldr, range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite, true)},
                [image](vk::CommandBuffer cmd) { postprocess::tonemap(cmd, image); });
        }
        else
        {
            graph.add("presentation",
                {gpu::access::for_image(target.ldr, range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eFragmentShader,
                    vk::AccessFlagBits::eShaderRead, false)},
                [image](vk::CommandBuffer cmd) { postprocess::present(cmd, image); });
        }
        graph.execute(command);
        graph.clear_passes();
    }

    void record(vk::CommandBuffer command, std::uint32_t image)
    {
        std::uint32_t available = resource::scene;
        const auto& list = ordered();
        for (std::uint32_t i = 0; i < list.size(); ++i)
        {
            if ((list[i].reads & available) != list[i].reads)
                throw std::runtime_error("Render pass has an unsatisfied resource dependency");
            step(command, image, i);
            available |= list[i].writes;
        }
    }
}
