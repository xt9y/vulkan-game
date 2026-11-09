#pragma once
#include "movement/detail.hpp"
#include <cmath>
#include "../player.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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
    struct peer {
        std::uint32_t slot=0;
        vk::util::math::vec3 position{};
    };
    struct peer_snapshot {
        std::uint32_t token=0,tick=0;
        std::vector<peer> players;
    };

inline input_packet encode(std::uint32_t token,const ::player::input& in) {
        input_packet p{'V','M','O','V',1,1,0,0};
        detail::write_fields(p.data()+8,token,in.sequence,in.buttons,in.yaw);
        return p;
    }
    inline std::optional<command> decode_input(const std::uint8_t* p,std::size_t n) {
        if(n!=24||!detail::header(p,n,1))return std::nullopt;
        command v{};
        detail::read_fields(p+8,v.token,v.input.sequence,v.input.buttons,v.input.yaw);
if(!v.token||!v.input.sequence|| (v.input.buttons&~::player::valid_buttons)|| !std::isfinite(v.input.yaw)||std::abs(v.input.yaw)>1000.f)
            return std::nullopt;
        return v;
    }
inline parameters_packet encode_parameters(std::uint32_t token, const ::player::parameters_t& v) {
        parameters_packet p{'V','M','O','V',1,3,0,0};
detail::write_fields(p.data()+8,token,v.spawn.x,v.spawn.y,v.spawn.z, v.walk_speed,v.sprint_speed,v.gravity,v.radius,v.height);
        return p;
    }
inline std::optional<player_parameters> decode_parameters(const std::uint8_t* p, std::size_t n) {
        if(n!=44||!detail::header(p,n,3))return std::nullopt;
        player_parameters v{};
        auto& x=v.values;
detail::read_fields(p+8,v.token,x.spawn.x,x.spawn.y,x.spawn.z, x.walk_speed,x.sprint_speed,x.gravity,x.radius,x.height);
if(!v.token||!std::isfinite(x.spawn.x)||!std::isfinite(x.spawn.y)|| !std::isfinite(x.spawn.z)||!std::isfinite(x.walk_speed)|| !std::isfinite(x.sprint_speed)||!std::isfinite(x.gravity)||
!std::isfinite(x.radius)||!std::isfinite(x.height)|| x.walk_speed<=0||x.sprint_speed<=0||x.gravity<=0|| x.radius<=0||x.height<=0)return std::nullopt;
        return v;
    }
    inline state_packet encode(const snapshot& s) {
        state_packet p{'V','M','O','V',1,2,0,0};
        const auto& x=s.player;
detail::write_fields(p.data()+8,s.token,s.sequence,s.tick, x.position.x,x.position.y,x.position.z, x.velocity.x,x.velocity.y,x.velocity.z, std::uint32_t(x.grounded));
        return p;
    }
inline std::optional<snapshot> decode_snapshot(const std::uint8_t* p, std::size_t n) {
        if(n!=48||!detail::header(p,n,2))return std::nullopt;
        snapshot s{};
        auto& x=s.player;
        std::uint32_t flag=0;
detail::read_fields(p+8,s.token,s.sequence,s.tick, x.position.x,x.position.y,x.position.z, x.velocity.x,x.velocity.y,x.velocity.z,flag);
        x.grounded=flag==1;
        const auto finite=[](float v){
            return std::isfinite(v)&&std::abs(v)<1000000.f;
        };
if(!s.token||flag>1|| !finite(x.position.x)||!finite(x.position.y)||!finite(x.position.z)|| !finite(x.velocity.x)||!finite(x.velocity.y)||!finite(x.velocity.z))
            return std::nullopt;
        return s;
    }

    inline std::vector<std::uint8_t> encode_peers(const peer_snapshot& snapshot) {
        std::vector<std::uint8_t> bytes{'V','M','O','V',1,4,0,0};
        bytes.resize(20+snapshot.players.size()*16);
        detail::write_fields(bytes.data()+8,snapshot.token,snapshot.tick,
                             std::uint32_t(snapshot.players.size()));
        for(std::size_t i=0;i<snapshot.players.size();++i) {
            const auto& peer=snapshot.players[i];
            detail::write_fields(bytes.data()+20+i*16,peer.slot,
                                 peer.position.x,peer.position.y,peer.position.z);
        }
        return bytes;
    }
    inline std::optional<peer_snapshot> decode_peers(const std::uint8_t* data,std::size_t size) {
        if(!detail::header(data,size,4)||size<20)return std::nullopt;
        peer_snapshot snapshot{};
        std::uint32_t count=0;
        detail::read_fields(data+8,snapshot.token,snapshot.tick,count);
        if(!snapshot.token||count>net::limits::max_clients||size!=20+std::size_t(count)*16)
            return std::nullopt;
        snapshot.players.reserve(count);
        for(std::uint32_t i=0;i<count;++i) {
            peer current{};
            detail::read_fields(data+20+i*16,current.slot,
                                current.position.x,current.position.y,current.position.z);
            const auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<1000000.f;};
            if(current.slot>=net::limits::max_clients ||
               !finite(current.position.x)||!finite(current.position.y)||!finite(current.position.z))
                return std::nullopt;
            snapshot.players.push_back(current);
        }
        return snapshot;
    }

}
