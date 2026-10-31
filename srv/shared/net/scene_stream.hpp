#pragma once

#include "srv/shared/scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace net::scene_stream
{
    inline constexpr std::size_t fragment_size = 1024;
    inline constexpr std::size_t header_size = 24;
    inline constexpr std::size_t packet_size = header_size + fragment_size;
    inline constexpr std::size_t max_scene_bytes = 8 * 1024 * 1024;

    using packet = std::array<std::uint8_t, packet_size>;

    struct fragment {
        std::uint32_t token = 0;
        std::uint32_t total = 0;
        std::uint32_t index = 0;
        std::uint32_t checksum = 0;
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
    };

    std::vector<std::uint8_t> serialize(const ::scene::state& scene);
    bool deserialize(const std::vector<std::uint8_t>& bytes, ::scene::state& scene);
    std::uint32_t checksum(const std::vector<std::uint8_t>& bytes);

    std::size_t encode_fragment(const std::vector<std::uint8_t>& bytes, std::uint32_t token, std::uint32_t index, std::uint32_t hash, packet& output);

    bool decode_fragment(const std::uint8_t* bytes, std::size_t size, fragment& output); 
} // namespace net::scene_stream
