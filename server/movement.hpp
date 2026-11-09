#pragma once
#include "player.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::movement {

    using input_packet=std::array<std::uint8_t,24>;
    using state_packet=std::array<std::uint8_t,48>;
    using parameters_packet=std::array<std::uint8_t,44>;
    struct player_parameters {std::uint32_t token;::player::parameters_t values;};
    struct command {std::uint32_t token=0;::player::input input;};
    struct snapshot {
        std::uint32_t token=0,sequence=0,tick=0;
        ::player::state player;
    };

    inline input_packet encode(std::uint32_t,const ::player::input&);
    inline std::optional<command> decode_input(const std::uint8_t*,std::size_t);
    inline parameters_packet encode_parameters(std::uint32_t,const ::player::parameters_t&);
    inline std::optional<player_parameters> decode_parameters(const std::uint8_t*,std::size_t);
    inline state_packet encode(const snapshot&);
    inline std::optional<snapshot> decode_snapshot(const std::uint8_t*,std::size_t);
}
