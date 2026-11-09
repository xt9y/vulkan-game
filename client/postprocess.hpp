#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::postprocess {
    void create();
    void synchronize_scene();
    void release();
    void present(vk::CommandBuffer command, std::uint32_t image);

} // namespace clt::postprocess
