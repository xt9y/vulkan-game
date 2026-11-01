#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::postprocess
{
    enum class effect { lighting, tonemapping };

    bool enabled(effect value);
    void set(effect value, bool enabled);
    void toggle(effect value);
    void set_exposure(float exposure);
    void create();
    void release();
    void lighting(vk::CommandBuffer command, std::uint32_t image);
    void tonemap(vk::CommandBuffer command, std::uint32_t image);
    void present(vk::CommandBuffer command, std::uint32_t image);
}
