#include "passes.hpp"
#include "postprocess.hpp"
#include "scene_renderer.hpp"

#include <stdexcept>
#include <vector>
#include <array>

namespace clt::passes
{
    namespace
    {
        struct state {
            std::vector<vk::QueryPool> queries;
            std::vector<bool> written;
            std::vector<bool> initialized;
            std::uint32_t valid_bits = 0;
            double period = 0;
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

    const std::array<description, 4>& ordered()
    {
        static const std::array<description, 4> list{{
            {"geometry", vk::PipelineBindPoint::eGraphics, resource::scene,
                resource::gbuffer, scene_renderer::draw},
            {"lighting", vk::PipelineBindPoint::eCompute, resource::gbuffer,
                resource::hdr, postprocess::lighting},
            {"tonemapping", vk::PipelineBindPoint::eCompute, resource::hdr,
                resource::ldr, postprocess::tonemap},
            {"presentation", vk::PipelineBindPoint::eGraphics, resource::ldr,
                resource::swapchain, postprocess::present}
        }};
        return list;
    }

    void create()
    {
        auto& data = get();
        const auto count = vk::swapchain::get()->images.size();
        data.initialized.assign(count, false);
        const auto family = vk::queue::get()->graphics_family;
        const auto queues = vk::physical_device::get()->handle.getQueueFamilyProperties();
        if (family >= queues.size() || !queues[family].timestampValidBits)
        {
            return;
        }

        data.valid_bits = queues[family].timestampValidBits;
        data.period = vk::physical_device::get()->handle.getProperties().limits.timestampPeriod;
        try
        {
            data.queries.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                vk::QueryPoolCreateInfo info{};
                info.queryType = vk::QueryType::eTimestamp;
                info.queryCount = 8;
                data.queries.push_back(vk::query::create(info));
            }
            data.written.assign(count, false);
        }
        catch (...)
        {
            release();  // Profiling is optional, not a renderer requirement.
        }
    }

    void release()
    {
        auto& data = get();
        for (auto pool : data.queries)
        {
            if (pool) vk::query::destroy(pool);
        }
        data = {};
    }

    void record(vk::CommandBuffer command, std::uint32_t image)
    {
        auto& data = get();
        const auto pool = image < data.queries.size() ? data.queries[image] :
            vk::QueryPool{};

        if (pool)
        {
            command.resetQueryPool(pool, 0, 8);
        }

        const auto& list = ordered();
        std::uint32_t available = resource::scene;
        for (std::uint32_t i = 0; i < list.size(); ++i)
        {
            if ((list[i].reads & available) != list[i].reads)
            {
                throw std::runtime_error("Render pass has an unsatisfied resource dependency");
            }
            if (pool)
            {
                command.writeTimestamp(
                    vk::PipelineStageFlagBits::eTopOfPipe, pool, 2 * i
                );
            }
            if (i == 1)
            {
                if (!data.initialized.at(image))
                {
                    const auto target = postprocess::images(image);
                    transition(command, target.hdr);
                    transition(command, target.ldr);
                    data.initialized[image] = true;
                }
                // The geometry render pass supplies its attachment-to-compute
                // external dependency and final sampled-image layouts.
            }
            else if (i == 2)
            {
                dependency(command,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,
                    vk::AccessFlagBits::eShaderRead);
            }
            else if (i == 3)
            {
                dependency(command,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::PipelineStageFlagBits::eFragmentShader,
                    vk::AccessFlagBits::eShaderWrite,
                    vk::AccessFlagBits::eShaderRead);
            }
            list[i].record(command, image);
            available |= list[i].writes;
            if (pool)
            {
                command.writeTimestamp(
                    vk::PipelineStageFlagBits::eBottomOfPipe, pool, 2 * i + 1
                );
            }
        }
        if (pool) data.written[image] = true;
    }

    timings read(std::uint32_t image)
    {
        auto& data = get();
        timings result;
        if (image >= data.queries.size() || !data.written[image])
        {
            return result;
        }

        std::array<std::uint64_t, 8> ticks{};
        const auto status = vk::device::get()->handle.getQueryPoolResults(
            data.queries[image], 0, 8, sizeof(ticks), ticks.data(),
            sizeof(std::uint64_t), vk::QueryResultFlagBits::e64
        );
        if (status != vk::Result::eSuccess)
        {
            return result;
        }

        const auto mask = data.valid_bits >= 64 ? ~std::uint64_t(0) :
            (std::uint64_t(1) << data.valid_bits) - 1;
        for (std::size_t i = 0; i < result.milliseconds.size(); ++i)
        {
            result.milliseconds[i] = double((ticks[2 * i + 1] -
                ticks[2 * i]) & mask) * data.period / 1e6;
        }
        result.valid = true;
        return result;
    }
}
