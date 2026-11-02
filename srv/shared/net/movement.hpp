#pragma once

#include "srv/shared/player/movement.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace net::movement
{
    using input_packet = std::array<std::uint8_t, 24>;
    using state_packet = std::array<std::uint8_t, 48>;
    using parameters_packet = std::array<std::uint8_t, 44>;

    struct player_parameters
    {
        std::uint32_t token;
        ::player::parameters_t values;
    };

    struct command {
        std::uint32_t token = 0;
        ::player::input input;
    };

    struct snapshot {
        std::uint32_t token = 0;
        std::uint32_t sequence = 0;
        std::uint32_t tick = 0;
        ::player::state player;
    };

    namespace detail
    {
        inline void put(std::uint8_t* p, std::uint32_t n)
        {
            p[0] = std::uint8_t(n >> 24);
            p[1] = std::uint8_t(n >> 16);
            p[2] = std::uint8_t(n >> 8);
            p[3] = std::uint8_t(n);
        }

        inline std::uint32_t get(const std::uint8_t* p)
        {
            return (std::uint32_t(p[0]) << 24) |
                   (std::uint32_t(p[1]) << 16) |
                   (std::uint32_t(p[2]) << 8) |
                   std::uint32_t(p[3]);
        }

        inline void put_float(std::uint8_t* p, float value)
        {
            std::uint32_t bits;
            std::memcpy(&bits, &value, 4);
            put(p, bits);
        }

        inline float get_float(const std::uint8_t* p)
        {
            const auto bits = get(p);
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
        }

        inline bool header(const std::uint8_t* p, std::size_t size, std::uint8_t kind)
        {
            return p && size >= 8 &&
                   p[0] == 'V' && p[1] == 'M' &&
                   p[2] == 'O' && p[3] == 'V' &&
                   p[4] == 1 && p[5] == kind &&
                   p[6] == 0 && p[7] == 0;
        }
    } // namespace detail

    inline input_packet encode(std::uint32_t token, const ::player::input& input)
    {
        input_packet bytes{'V', 'M', 'O', 'V', 1, 1, 0, 0};

        detail::put(bytes.data() + 8, token);
        detail::put(bytes.data() + 12, input.sequence);
        detail::put(bytes.data() + 16, input.buttons);
        detail::put_float(bytes.data() + 20, input.yaw);

        return bytes;
    }

    inline std::optional<command> decode_input(const std::uint8_t* p, std::size_t size)
    {
        if (size != 24 || !detail::header(p, size, 1))
        {
            return std::nullopt;
        }

        command value;
        value.token = detail::get(p + 8);
        value.input.sequence = detail::get(p + 12);
        value.input.buttons = detail::get(p + 16);
        value.input.yaw = detail::get_float(p + 20);

        if (!value.token || !value.input.sequence ||
            (value.input.buttons & ~::player::valid_buttons) ||
            !std::isfinite(value.input.yaw) ||
            std::abs(value.input.yaw) > 1000.0f)
        {
            return std::nullopt;
        }

        return value;
    }

    inline parameters_packet encode_parameters(std::uint32_t token,
                                               const ::player::parameters_t& settings)
    {
        parameters_packet bytes{'V', 'M', 'O', 'V', 1, 3, 0, 0};
        detail::put(bytes.data() + 8, token);
        detail::put_float(bytes.data() + 12, settings.spawn.x);
        detail::put_float(bytes.data() + 16, settings.spawn.y);
        detail::put_float(bytes.data() + 20, settings.spawn.z);
        detail::put_float(bytes.data() + 24, settings.walk_speed);
        detail::put_float(bytes.data() + 28, settings.sprint_speed);
        detail::put_float(bytes.data() + 32, settings.gravity);
        detail::put_float(bytes.data() + 36, settings.radius);
        detail::put_float(bytes.data() + 40, settings.height);
        return bytes;
    }

    inline std::optional<player_parameters> decode_parameters(const std::uint8_t* p,
                                                               std::size_t size)
    {
        if (size != parameters_packet{}.size() || !detail::header(p, size, 3))
            return std::nullopt;
        player_parameters value{};
        value.token = detail::get(p + 8);
        auto& v = value.values;
        v.spawn.x = detail::get_float(p + 12);
        v.spawn.y = detail::get_float(p + 16);
        v.spawn.z = detail::get_float(p + 20);
        v.walk_speed = detail::get_float(p + 24);
        v.sprint_speed = detail::get_float(p + 28);
        v.gravity = detail::get_float(p + 32);
        v.radius = detail::get_float(p + 36);
        v.height = detail::get_float(p + 40);
        if (!value.token || !std::isfinite(v.spawn.x) || !std::isfinite(v.spawn.y) ||
            !std::isfinite(v.spawn.z) || !std::isfinite(v.walk_speed) ||
            !std::isfinite(v.sprint_speed) || !std::isfinite(v.gravity) ||
            !std::isfinite(v.radius) || !std::isfinite(v.height) ||
            v.walk_speed <= 0 || v.sprint_speed <= 0 || v.gravity <= 0 ||
            v.radius <= 0 || v.height <= 0)
            return std::nullopt;
        return value;
    }

    inline state_packet encode(const snapshot& state)
    {
        state_packet bytes{'V', 'M', 'O', 'V', 1, 2, 0, 0};

        detail::put(bytes.data() + 8, state.token);
        detail::put(bytes.data() + 12, state.sequence);
        detail::put(bytes.data() + 16, state.tick);

        const auto& p = state.player;

        detail::put_float(bytes.data() + 20, p.position.x);
        detail::put_float(bytes.data() + 24, p.position.y);
        detail::put_float(bytes.data() + 28, p.position.z);

        detail::put_float(bytes.data() + 32, p.velocity.x);
        detail::put_float(bytes.data() + 36, p.velocity.y);
        detail::put_float(bytes.data() + 40, p.velocity.z);

        detail::put(bytes.data() + 44, p.grounded ? 1 : 0);

        return bytes;
    }

    inline std::optional<snapshot> decode_snapshot(const std::uint8_t* p, std::size_t size)
    {
        if (size != 48 || !detail::header(p, size, 2))
        {
            return std::nullopt;
        }

        snapshot value;
        value.token = detail::get(p + 8);
        value.sequence = detail::get(p + 12);
        value.tick = detail::get(p + 16);

        auto& player = value.player;

        player.position.x = detail::get_float(p + 20);
        player.position.y = detail::get_float(p + 24);
        player.position.z = detail::get_float(p + 28);

        player.velocity.x = detail::get_float(p + 32);
        player.velocity.y = detail::get_float(p + 36);
        player.velocity.z = detail::get_float(p + 40);

        const auto flag = detail::get(p + 44);
        player.grounded = flag == 1;

        const auto finite = [](float x)
        {
            return std::isfinite(x) && std::abs(x) < 1000000;
        };

        if (!value.token || flag > 1 ||
            !finite(player.position.x) ||
            !finite(player.position.y) ||
            !finite(player.position.z) ||
            !finite(player.velocity.x) ||
            !finite(player.velocity.y) ||
            !finite(player.velocity.z))
        {
            return std::nullopt;
        }

        return value;
    }
} // namespace net::movement
