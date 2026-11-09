#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::message {
enum class type : std::uint8_t {
hello = 1, welcome, ping, state, goodbye, scene_request
    };
    struct resource {
        message::type type;
        std::uint32_t sequence = 0;
        std::uint32_t token = 0;
        std::uint32_t value = 0;
    };
    inline constexpr std::size_t packet_size = 20;
    using packet = std::array<std::uint8_t, packet_size>;
    packet encode(const resource& message);
    std::optional<resource> decode(const std::uint8_t* data, std::size_t size);
    bool newer(std::uint32_t sequence, std::uint32_t previous);
}
