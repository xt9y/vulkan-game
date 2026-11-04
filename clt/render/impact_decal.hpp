#pragma once
#include <cstdint>
#include <vk/vk.hpp>

namespace clt::impact_decal
{
    void create();
    void record(vk::CommandBuffer command, std::uint32_t image);
    vk::ImageView overlay_view(std::uint32_t image);
    void release();
} // namespace clt::impact_decal
