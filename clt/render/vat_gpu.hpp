#pragma once

#include <array>
#include <cstdint>
#include <vk/vk.hpp>

namespace clt::vat_gpu
{
    struct alignas(16) parameters_t
    {
        std::array<float, 4> timing{};   // seconds, fps, frames, mirror
        std::array<float, 4> bounds_min{};
        std::array<float, 4> bounds_max{};
    };
    static_assert(sizeof(parameters_t) == 48);

    bool prepared(std::uint32_t type);
    void init();
    void bind(std::uint32_t type, vk::DescriptorSet set);
    parameters_t parameters(std::uint32_t type, float age, bool mirror);
    void shutdown() noexcept;
} // namespace clt::vat_gpu
