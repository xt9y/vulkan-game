#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::postprocess
{
    struct settings {
        bool lighting = true;
        bool tonemapping = true;
        float exposure = 1.0f;
    };

    settings& options();
    void create();
    void release();
    void lighting(vk::CommandBuffer command, std::uint32_t image);
    void tonemap(vk::CommandBuffer command, std::uint32_t image);
    void present(vk::CommandBuffer command, std::uint32_t image);
}
