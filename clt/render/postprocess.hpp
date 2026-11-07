#pragma once
#include <vk/vk.hpp>
#include "clt/view/camera.hpp"
#include <cstdint>

namespace clt::postprocess
{
    void set_exposure(float exposure);
    struct targets {
        vk::Image indirect{};
        vk::Image hdr{};
        vk::Image ldr{};
    };

    targets images(std::uint32_t image);
    void create();
    void synchronize_lights();
    void release();
    void gi(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void lighting(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void tonemap(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void present(vk::CommandBuffer command, std::uint32_t image);
}
