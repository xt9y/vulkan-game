#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::postprocess
{
    void set_exposure(float exposure);
    struct targets {
        vk::Image hdr{};
        vk::Image ldr{};
    };

    targets images(std::uint32_t image);
    void create();
    void release();
    void lighting(vk::CommandBuffer command, std::uint32_t image);
    void tonemap(vk::CommandBuffer command, std::uint32_t image);
    void present(vk::CommandBuffer command, std::uint32_t image);
}
