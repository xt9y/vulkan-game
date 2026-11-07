#pragma once
#include <vk/vk.hpp>
#include "clt/view/camera.hpp"
#include <cstdint>

namespace clt::postprocess
{
    void set_exposure(float exposure);
    struct targets {
        vk::Image grid{};
        vk::Buffer points{};
        vk::Image hdr{};
        vk::Image ldr{};
    };

    targets images(std::uint32_t image);
    void create();
    void synchronize_scene();
    bool needs_update();
    void updated();
    void toggle_probes();
    void release();
    void cloud(vk::CommandBuffer command,std::uint32_t image);
    void gi(vk::CommandBuffer command,std::uint32_t image);
    void lighting(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void tonemap(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void present(vk::CommandBuffer command, std::uint32_t image);
}
