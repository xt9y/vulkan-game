#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::blood
{
    void create();
    void record(vk::CommandBuffer command, std::uint32_t image);
    vk::ImageView overlay(std::uint32_t image);
    void release();
}
