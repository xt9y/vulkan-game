#pragma once
#include <vk/vk.hpp>
#include <array>
#include <cstdint>

namespace clt::passes
{
    enum class stage : std::uint32_t { geometry, lighting, tonemapping, presentation, count };

    enum resource : std::uint32_t {
        scene = 1, gbuffer = 2, hdr = 4, ldr = 8, swapchain = 16
    };

    struct description {
        const char* name;
        vk::PipelineBindPoint queue;
        std::uint32_t reads;
        std::uint32_t writes;
        void (*record)(vk::CommandBuffer, std::uint32_t);
    };

    const std::array<description, 4>& ordered();
    void create();
    void release();
    void record(vk::CommandBuffer command, std::uint32_t image);
    // Record one pass, including its resource transitions. Call in order.
    void step(vk::CommandBuffer command, std::uint32_t image, std::uint32_t index);
}
