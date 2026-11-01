#pragma once

#include "srv/shared/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace clt::animation
{
    using matrix = std::array<float, 16>;

    // Only called on the Vulkan render thread.
    void synchronize(const ::scene::state& scene);
    std::uint32_t joints(std::uint32_t skin_id);
    bool sample(std::uint32_t skin_id, std::uint32_t clip,
                float seconds, std::vector<matrix>& palette);
    void reset();
} // namespace clt::animation
