#pragma once
#include "../player.hpp"
#include "movement/detail.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace net::movement
{
using input_packet=std::array<std::uint8_t,24>;
using state_packet=std::array<std::uint8_t,48>;
using parameters_packet=std::array<std::uint8_t,44>;

struct player_parameters {
    std::uint32_t token;
    ::player::parameters_t values;
};
struct command {
    std::uint32_t token=0;
    ::player::input input;
};
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

namespace {
template<std::size_t N,class... T>
std::array<std::uint8_t,N> fixed_packet(std::uint8_t type,const T&... fields)
{
    static_assert(N==8+4*sizeof...(T));
    std::array<std::uint8_t,N> result{'V','M','O','V',1,type,0,0};
    detail::write_fields(result.data()+8,fields...);
    return result;
}
bool bounded(float v) {return std::isfinite(v)&&std::abs(v)<1000000.f;}
bool valid_position(vk::util::math::vec3 v) {
    return bounded(v.x)&&bounded(v.y)&&bounded(v.z);
}
}

input_packet encode(std::uint32_t token,const ::player::input& in)
{
    return fixed_packet<24>(1,token,in.sequence,in.buttons,in.yaw);
}
std::optional<command> decode_input(const std::uint8_t* p,std::size_t n)
{
    if(n!=24 || !detail::header(p,n,1))return std::nullopt;
    command v{};
    detail::read_fields(p+8,v.token,v.input.sequence,v.input.buttons,v.input.yaw);
    if(!v.token||!v.input.sequence||(v.input.buttons & ~::player::valid_buttons)||
       !std::isfinite(v.input.yaw)||std::abs(v.input.yaw)>1000.f)return std::nullopt;
    return v;
}
parameters_packet encode_parameters(std::uint32_t token,const ::player::parameters_t& v)
{
    return fixed_packet<44>(3,token,v.spawn.x,v.spawn.y,v.spawn.z,
        v.walk_speed,v.sprint_speed,v.gravity,v.radius,v.height);
}
std::optional<player_parameters> decode_parameters(const std::uint8_t* p,std::size_t n)
{
    if(n!=44 || !detail::header(p,n,3))return std::nullopt;
    player_parameters v{};
    auto& x=v.values;
    detail::read_fields(p+8,v.token,x.spawn.x,x.spawn.y,x.spawn.z,
        x.walk_speed,x.sprint_speed,x.gravity,x.radius,x.height);
    if(!v.token || !std::isfinite(x.spawn.x) || !std::isfinite(x.spawn.y) ||
       !std::isfinite(x.spawn.z) || !std::isfinite(x.walk_speed) ||
       !std::isfinite(x.sprint_speed) || !std::isfinite(x.gravity) ||
       !std::isfinite(x.radius) || !std::isfinite(x.height) ||
       x.walk_speed<=0 || x.sprint_speed<=0 || x.gravity<=0 ||
       x.radius<=0 || x.height<=0)return std::nullopt;
    return v;
}
state_packet encode(const snapshot& s)
{
    const auto& x=s.player;
    return fixed_packet<48>(2,s.token,s.sequence,s.tick,
        x.position.x,x.position.y,x.position.z,
        x.velocity.x,x.velocity.y,x.velocity.z,std::uint32_t(x.grounded));
}
std::optional<snapshot> decode_snapshot(const std::uint8_t* p,std::size_t n)
{
    if(n!=48 || !detail::header(p,n,2))return std::nullopt;
    snapshot s{};
    auto& x=s.player;
    std::uint32_t flag=0;
    detail::read_fields(p+8,s.token,s.sequence,s.tick,
        x.position.x,x.position.y,x.position.z,
        x.velocity.x,x.velocity.y,x.velocity.z,flag);
    x.grounded=flag==1;
    if(!s.token||flag>1||!valid_position(x.position)||!valid_position(x.velocity))
        return std::nullopt;
    return s;
}
std::vector<std::uint8_t> encode_peers(const peer_snapshot& snapshot)
{
    std::vector<std::uint8_t> data{'V','M','O','V',1,4,0,0};
    data.resize(20+snapshot.players.size()*16);
    detail::write_fields(data.data()+8,snapshot.token,snapshot.tick,
                         std::uint32_t(snapshot.players.size()));
    for(std::size_t i=0;i<snapshot.players.size();++i) {
        const auto& p=snapshot.players[i];
        detail::write_fields(data.data()+20+i*16,p.slot,
                             p.position.x,p.position.y,p.position.z);
    }
    return data;
}
std::optional<peer_snapshot> decode_peers(const std::uint8_t* data,std::size_t size)
{
    if(size<20 || !detail::header(data,size,4))return std::nullopt;
    peer_snapshot result{};
    std::uint32_t count=0;
    detail::read_fields(data+8,result.token,result.tick,count);
    if(!result.token||count>net::limits::max_clients||
       size!=20+std::size_t(count)*16)return std::nullopt;
    result.players.reserve(count);
    for(std::uint32_t i=0;i<count;++i) {
        peer current{};
        detail::read_fields(data+20+i*16,current.slot,
                            current.position.x,current.position.y,current.position.z);
        if(current.slot>=net::limits::max_clients ||
           !valid_position(current.position))return std::nullopt;
        result.players.push_back(current);
    }
    return result;
}
} // namespace net::movement
