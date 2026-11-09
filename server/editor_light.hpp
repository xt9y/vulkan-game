#pragma once
#include "scene.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace net::editor_light {

    enum class kind : std::uint8_t {
        create=1,edit=2,state=3,ack=4,remove=5
    };
    struct resource {
        kind type=kind::state;
        std::uint32_t token=0,sequence=0,index=0,revision=0;
        ::scene::point_light light{};
    };
    using packet=std::array<std::uint8_t,56>;
    inline constexpr std::array<std::string_view,8> property_keys{
        "pos_x","pos_y","pos_z","color_r","color_g","color_b","radius","strength"
    };
    template<class Light>
    auto property_fields(Light& light) {
return std::array{&light.position.x,&light.position.y,&light.position.z, &light.color.x,&light.color.y,&light.color.z,&light.radius,&light.strength};
    }

    inline bool valid(const ::scene::point_light&);
    inline packet encode(const resource&);
    inline std::optional<resource> decode(const std::uint8_t*,std::size_t);
}
