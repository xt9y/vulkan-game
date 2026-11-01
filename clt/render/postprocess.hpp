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
    void configure_options(bool visible, int selected);
    bool options_open();
    void toggle_options();
    void options_select(int change);
    void options_adjust(int change);
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
