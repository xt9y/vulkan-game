#include "message.hpp"

namespace net::message
{
    namespace
    {
        void write(std::uint8_t* data, std::uint32_t value)
        {
            for (int i = 3; i >= 0; --i)
            {
                data[i] = static_cast<std::uint8_t>(value);
                value >>= 8;
            }
        }

        std::uint32_t read(const std::uint8_t* data)
        {
            return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16) |
                   (std::uint32_t(data[2]) << 8) | data[3];
        }
    } // namespace

    packet encode(const resource& message)
    {
        packet result{'V', 'G', 'A', 'M', 1, static_cast<std::uint8_t>(message.type), 0, 0};
        write(result.data() + 8, message.sequence);
        write(result.data() + 12, message.token);
        write(result.data() + 16, message.value);

        return result;
    }

    std::optional<resource> decode(const std::uint8_t* data, std::size_t size)
    {
        if (size != packet_size || data[0] != 'V' || data[1] != 'G' || data[2] != 'A' ||
            data[3] != 'M' || data[4] != 1 || data[5] < 1 || data[5] > 5 || data[6] || data[7])
        {
            return std::nullopt;
        }

        return resource{
            static_cast<type>(data[5]), read(data + 8), read(data + 12), read(data + 16)
        };
    }

    bool newer(std::uint32_t sequence, std::uint32_t previous)
    {
        const auto difference = sequence - previous;

        return difference != 0 && difference < 0x80000000u;
    }
} // namespace net::message
