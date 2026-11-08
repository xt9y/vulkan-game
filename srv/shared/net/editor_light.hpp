#pragma once

#include "movement.hpp"
#include "srv/shared/scene/scene.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::editor_light
{
    enum class kind : std::uint8_t { create=1,edit=2,state=3,ack=4,remove=5 };
    struct resource
    {
        kind type=kind::state;
        std::uint32_t token=0;
        std::uint32_t sequence=0; // Command sequence, or state revision.
        std::uint32_t index=0;    // Server-authoritative light index.
        std::uint32_t revision=0; // State revision, including acknowledgements.
        ::scene::point_light light{};
    };
    using packet=std::array<std::uint8_t,56>;

    inline bool valid(const ::scene::point_light& l)
    {
        const auto finite=[](float v,float limit)
        {
            return std::isfinite(v)&&std::abs(v)<=limit;
        };
        return finite(l.position.x,500.0f) &&
               finite(l.position.y,500.0f) &&
               finite(l.position.z,500.0f) &&
               finite(l.color.x,1.0f) && l.color.x>=0 &&
               finite(l.color.y,1.0f) && l.color.y>=0 &&
               finite(l.color.z,1.0f) && l.color.z>=0 &&
               finite(l.radius,64.0f) && l.radius>=0.05f &&
               finite(l.strength,20.0f) && l.strength>=0;
    }

    inline packet encode(const resource& item)
    {
        using namespace net::movement::detail;
        packet result{'V','L','I','T',1,
            static_cast<std::uint8_t>(item.type),
            static_cast<std::uint8_t>(item.light.enabled?1:0),0};
        auto* p=result.data();
        put(p+8,item.token);
        put(p+12,item.sequence);
        put(p+16,item.index);
        put(p+20,item.revision);
        put_float(p+24,item.light.position.x);
        put_float(p+28,item.light.position.y);
        put_float(p+32,item.light.position.z);
        put_float(p+36,item.light.radius);
        put_float(p+40,item.light.color.x);
        put_float(p+44,item.light.color.y);
        put_float(p+48,item.light.color.z);
        put_float(p+52,item.light.strength);
        return result;
    }

    inline std::optional<resource> decode(const std::uint8_t* p,
                                          std::size_t size)
    {
        if(!p || size!=packet{}.size() ||
           p[0]!='V'||p[1]!='L'||p[2]!='I'||p[3]!='T'||
           p[4]!=1||p[5]<1||p[5]>5||p[6]>1||p[7]!=0)
            return std::nullopt;
        using namespace net::movement::detail;
        resource item{};
        item.type=kind(p[5]);
        item.token=get(p+8);
        item.sequence=get(p+12);
        item.index=get(p+16);
        item.revision=get(p+20);
        item.light.position={get_float(p+24),get_float(p+28),get_float(p+32)};
        item.light.radius=get_float(p+36);
        item.light.color={get_float(p+40),get_float(p+44),get_float(p+48)};
        item.light.strength=get_float(p+52);
        item.light.enabled=p[6]!=0;
        if(!item.token || !item.sequence || !valid(item.light))
            return std::nullopt;
        if(item.type==kind::create && item.index!=UINT32_MAX)
            return std::nullopt;
        if(item.type!=kind::create && item.index>=64)
            return std::nullopt;
        return item;
    }
}
