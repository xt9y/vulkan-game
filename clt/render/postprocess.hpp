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
    struct targets {
        vk::Image hdr{};
        vk::Image ldr{};
    };

    targets images(std::uint32_t image);
    vk::ImageView hdr_view(std::uint32_t image);
    vk::Sampler sampler();
    void create();
    void release();
    void lighting(vk::CommandBuffer command, std::uint32_t image);
    void tonemap(vk::CommandBuffer command, std::uint32_t image);
    void present(vk::CommandBuffer command, std::uint32_t image);
}
