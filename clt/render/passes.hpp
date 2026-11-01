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

    struct timings {
        std::array<double, 4> milliseconds{};
        bool valid = false;
    };

    const std::array<description, 4>& ordered();
    void create();
    void release();
    void record(vk::CommandBuffer command, std::uint32_t image);
    // Caller must have waited for the fence of the previous submission for image.
    timings read(std::uint32_t image);
}
