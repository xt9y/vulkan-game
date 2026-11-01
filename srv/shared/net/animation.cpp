#include "animation.hpp"

#include <cmath>
#include <cstring>

namespace net::animation
{
    namespace
    {
        void write(std::uint8_t* out, std::uint32_t value)
        {
            out[0] = std::uint8_t(value >> 24);
            out[1] = std::uint8_t(value >> 16);
            out[2] = std::uint8_t(value >> 8);
            out[3] = std::uint8_t(value);
        }

        std::uint32_t read(const std::uint8_t* in)
        {
            return (std::uint32_t(in[0]) << 24) |
                (std::uint32_t(in[1]) << 16) |
                (std::uint32_t(in[2]) << 8) | std::uint32_t(in[3]);
        }
    }

    packet encode(const resource& state)
    {
        packet bytes{};
        bytes[0] = 'A';
        bytes[1] = 'N';
        bytes[2] = 'I';
        bytes[3] = 'M';
        write(bytes.data() + 4, 1);
        write(bytes.data() + 8, state.token);
        write(bytes.data() + 12, state.server_tick);
        write(bytes.data() + 16, state.instance_id);
        write(bytes.data() + 20, state.clip);
        write(bytes.data() + 24, state.start_tick);
        write(bytes.data() + 28, state.overlay);
        write(bytes.data() + 32, state.mask_root);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &state.blend, sizeof(bits));
        write(bytes.data() + 36, bits);
        return bytes;
    }

    std::optional<resource> decode(const std::uint8_t* bytes, std::size_t size)
    {
        if (!bytes || size != packet{}.size() ||
            bytes[0] != 'A' || bytes[1] != 'N' ||
            bytes[2] != 'I' || bytes[3] != 'M' ||
            read(bytes + 4) != 1)
            return std::nullopt;

        resource result;
        result.token = read(bytes + 8);
        result.server_tick = read(bytes + 12);
        result.instance_id = read(bytes + 16);
        result.clip = read(bytes + 20);
        result.start_tick = read(bytes + 24);
        result.overlay = read(bytes + 28);
        result.mask_root = read(bytes + 32);
        const auto bits = read(bytes + 36);
        std::memcpy(&result.blend, &bits, sizeof(result.blend));
        if (!result.token || !result.instance_id ||
            !std::isfinite(result.blend) ||
            result.blend < 0 || result.blend > 1)
            return std::nullopt;
        return result;
    }
    event_packet encode_event(const attachment_event& event)
    {
        event_packet bytes{};
        bytes[0] = 'E';
        bytes[1] = 'V';
        bytes[2] = 'N';
        bytes[3] = 'T';
        write(bytes.data() + 4, event.token);
        write(bytes.data() + 8, event.tick);
        write(bytes.data() + 12, event.sequence);
        write(bytes.data() + 16, event.instance_id);
        write(bytes.data() + 20, event.joint);
        write(bytes.data() + 24, event.kind);
        return bytes;
    }

    std::optional<attachment_event> decode_event(const std::uint8_t* bytes,
                                                  std::size_t size)
    {
        if (!bytes || size != event_packet{}.size() ||
            bytes[0] != 'E' || bytes[1] != 'V' ||
            bytes[2] != 'N' || bytes[3] != 'T')
            return std::nullopt;
        attachment_event event;
        event.token = read(bytes + 4);
        event.tick = read(bytes + 8);
        event.sequence = read(bytes + 12);
        event.instance_id = read(bytes + 16);
        event.joint = read(bytes + 20);
        event.kind = read(bytes + 24);
        return event.token && event.instance_id && event.sequence ?
            std::optional<attachment_event>(event) : std::nullopt;
    }
} // namespace net::animation
