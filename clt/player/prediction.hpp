#pragma once

#include "srv/shared/player/movement.hpp"

namespace clt::prediction
{
    void init();
    void controls(std::uint32_t buttons, float yaw);
    vk::util::math::vec3 position();
    void shutdown();
} // namespace clt::prediction
