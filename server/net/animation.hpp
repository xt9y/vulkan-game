#pragma once
#include "movement/detail.hpp"
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::animation {
struct resource {
        std::uint32_t token = 0;
        std::uint32_t server_tick = 0;
        std::uint32_t instance_id = 0;
        std::uint32_t clip = 0;
        std::uint32_t start_tick = 0;
        std::uint32_t overlay = UINT32_MAX;
        std::uint32_t mask_root = UINT32_MAX;
        float blend = 0;
    };
    using packet = std::array<std::uint8_t, 40>;
    packet encode(const resource& state);
    std::optional<resource> decode(const std::uint8_t* data, std::size_t bytes);
packet encode(const resource& state) {
        packet p{'A','N','I','M'};
net::movement::detail::write_fields(p.data()+4,1u,state.token,state.server_tick, state.instance_id,state.clip,state.start_tick,state.overlay, state.mask_root,state.blend);
        return p;
    }
    std::optional<resource> decode(const std::uint8_t* p,std::size_t n) {
if(!p||n!=packet{}.size()||p[0]!='A'||p[1]!='N'|| p[2]!='I'||p[3]!='M'||net::movement::detail::get(p+4)!=1u)
            return std::nullopt;
        resource v{};
net::movement::detail::read_fields(p+8,v.token,v.server_tick,v.instance_id, v.clip,v.start_tick,v.overlay,v.mask_root,v.blend);
if(!v.token||!v.instance_id||!std::isfinite(v.blend)|| v.blend<0||v.blend>1)return std::nullopt;
        return v;
    }

}
