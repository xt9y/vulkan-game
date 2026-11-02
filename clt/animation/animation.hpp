#pragma once

#include "srv/shared/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <vector>
#include <string_view>

namespace clt::animation
{
    using matrix = std::array<float, 16>;

    // Only called on the Vulkan render thread.
    void synchronize(const ::scene::state& scene);
    std::uint32_t joints(std::uint32_t skin_id);
    bool sample(
        std::uint32_t skin_id,
        std::uint32_t clip,
        float seconds,
        std::vector<matrix>& palette,
        std::uint32_t overlay = UINT32_MAX,
        float weight = 0.0f,
        std::uint32_t mask_root = UINT32_MAX,
        bool loop = true
    );
    float duration(std::uint32_t skin_id, std::uint32_t clip);
    // Read after sample() on the same render thread for weapon sockets/effects.
    std::uint32_t joint(std::uint32_t skin_id, std::string_view name);
    bool joint_transform(std::uint32_t skin_id, std::uint32_t index, matrix& out);
    bool offset_joint(
        std::uint32_t skin_id,
        std::uint32_t root,
        vk::util::math::vec3 model_offset,
        std::vector<matrix>& palette
    );
    void reset();
} // namespace clt::animation
