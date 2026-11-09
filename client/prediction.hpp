#pragma once
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::prediction {
    void init();
    void controls(std::uint32_t buttons, float yaw);
    vk::util::math::vec3 position();
    std::uint64_t ticks();
    void shutdown();

} // namespace clt::prediction
