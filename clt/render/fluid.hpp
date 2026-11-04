#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::fluid
{
    void create();
    void record(vk::CommandBuffer command,std::uint32_t image);
    bool active();
    vk::ImageView output(std::uint32_t image);
    void release();
}
