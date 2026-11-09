#pragma once
#include "../scene.hpp"
#include "movement/detail.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace net::editor_cube {
enum class kind: std::uint8_t {create=1,edit=2,state=3,ack=4,remove=5};
inline constexpr std::array<std::string_view,13> property_keys{
    "pos_x","pos_y","pos_z","rot_x","rot_y","rot_z",
    "width","height","depth","texture","color_r","color_g","color_b"
};
inline vk::util::math::vec3 angles(const ::scene::instance& cube) {return cube.angles;}
inline void update_rotation(::scene::instance& cube) {
    constexpr float radians=0.01745329251994329577f;
    cube.pose.rotation=vk::util::math::euler(
        cube.angles.x*radians,cube.angles.y*radians,cube.angles.z*radians);
}
inline bool valid(const ::scene::instance& cube) {
    const auto finite=[](float x,float lo,float hi) {
        return std::isfinite(x) && x>=lo && x<=hi;
    };
    const auto& p=cube.pose.position;
    const auto& s=cube.pose.scale;
    const auto& a=cube.angles;
    const auto& c=cube.tint;
    return finite(p.x,-500,500)&&finite(p.y,-500,500)&&finite(p.z,-500,500)
        &&finite(s.x,.01f,100)&&finite(s.y,.01f,100)&&finite(s.z,.01f,100)
        &&finite(a.x,-180,180)&&finite(a.y,-180,180)&&finite(a.z,-180,180)
        &&finite(c.x,0,1)&&finite(c.y,0,1)&&finite(c.z,0,1)
        &&cube.pattern<=1;
}
inline bool set(::scene::instance& cube,std::uint32_t field,double value) {
    if(field>=property_keys.size() || !std::isfinite(value))return false;
    float* fields[]{
        &cube.pose.position.x,&cube.pose.position.y,&cube.pose.position.z,
        &cube.angles.x,&cube.angles.y,&cube.angles.z,
        &cube.pose.scale.x,&cube.pose.scale.y,&cube.pose.scale.z,
        nullptr,&cube.tint.x,&cube.tint.y,&cube.tint.z
    };
    if(field==9) {
        if(value!=0 && value!=1)return false;
        cube.pattern=std::uint32_t(value);
    }else *fields[field]=float(field>=10?value/100.0:value);
    if(!valid(cube))return false;
    update_rotation(cube);
    return true;
}
struct resource {
    kind type=kind::state;
    std::uint32_t token=0,sequence=0,id=0,revision=0;
    ::scene::instance cube{};
};
using packet=std::array<std::uint8_t,80>;
inline packet encode(const resource& v) {
    packet p{'C','U','B','E',1,std::uint8_t(v.type),0,0};
    const auto& c=v.cube;
    net::movement::detail::write_fields(p.data()+8,
        v.token,v.sequence,v.id,v.revision,
        c.pose.position.x,c.pose.position.y,c.pose.position.z,
        c.pose.scale.x,c.pose.scale.y,c.pose.scale.z,
        c.angles.x,c.angles.y,c.angles.z,
        c.tint.x,c.tint.y,c.tint.z,c.pattern,std::uint32_t(c.enabled));
    return p;
}
inline std::optional<resource> decode(const std::uint8_t* p,std::size_t n) {
    if(!p||n!=packet{}.size()||p[0]!='C'||p[1]!='U'||p[2]!='B'||p[3]!='E'
        ||p[4]!=1||p[5]<1||p[5]>5||p[6]||p[7])return std::nullopt;
    resource v{};
    v.type=kind(p[5]);
    auto& c=v.cube;
    std::uint32_t enabled=0;
    net::movement::detail::read_fields(p+8,
        v.token,v.sequence,v.id,v.revision,
        c.pose.position.x,c.pose.position.y,c.pose.position.z,
        c.pose.scale.x,c.pose.scale.y,c.pose.scale.z,
        c.angles.x,c.angles.y,c.angles.z,
        c.tint.x,c.tint.y,c.tint.z,c.pattern,enabled);
    c.enabled=enabled!=0;
    c.editor_cube=true;
    c.id=v.id;
    if(!v.token||!v.sequence||enabled>1||!valid(c)
       ||(v.type==kind::create?v.id!=UINT32_MAX:v.id==UINT32_MAX))
        return std::nullopt;
    update_rotation(c);
    return v;
}
} // namespace net::editor_cube
