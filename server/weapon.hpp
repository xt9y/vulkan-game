#pragma once
#include "message.hpp"
#include "scene.hpp"
#include <spatial/index.hpp>
#include <vk/vk.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace weapon {
    using vk::util::math::vec3;
    enum class action : std::uint32_t {
fire = 1, reload = 2, equip = 3
    };
    enum class reload_phase : std::uint32_t {
none, tactical, empty
    };
    enum class result : std::uint32_t {
none, fired, cooldown, empty, reloading, reload_started, reload_unnecessary, equipped
    };
    struct timing {
        float reload_duration = 0;
        float empty_reload_duration = 0;
        float refill_time = 0;
        float empty_refill_time = 0;
    };
    struct definition {
        std::uint32_t id = 0;
        std::uint32_t magazine_capacity = 0;
        std::uint32_t initial_reserve = 0;
        double fire_interval = 0;
        float range = 0;
        timing reload;
    };

    struct state {
        std::uint32_t magazine = 0;
        std::uint32_t reserve = 0;
        std::uint32_t sequence = 0;
        reload_phase reload = reload_phase::none;
        result last_result = result::none;
        double next_fire = 0;
        double reload_started = 0;
        double reload_finishes = 0;
        double refill_at = 0;
        bool refilled = false;
    };

    struct triangle {
        vec3 a, b, c;
        std::uint32_t instance = 0, index = 0;
    };
    struct hit {
        bool impacted = false;
        vec3 position{}, normal{};
        std::uint32_t instance = 0, triangle = 0;
    };
    struct world {
        std::vector<triangle> triangles;
        spatial::index index;
    };


    enum class motion {
idle,walk,draw,reload,reload_empty, ads_idle,ads_in,ads_out,ads_walk, fire1,fire2,fire3,ads_fire1,ads_fire2,ads_fire3,count
    };
    struct clip {
        std::string name;
        float speed=1.f;
        bool loop=false;
    };
    struct bindings {
        int trigger_button=0,aim_button=1;
        int reload_key='R',inspect_key='M';
        bool automatic=false;
        float sensitivity=.0025f;
        float ads_sensitivity_multiplier=.60f;
    };
    struct gun {
        definition rules;
        bindings input{};
        std::filesystem::path model,casing_model;
        std::array<clip,std::size_t(motion::count)> animations{};
        std::string camera_joint,muzzle_joint,ejection_joint,slide_joint;
        vk::util::math::vec3 muzzle_offset{0.f,0.f,-.4f};
        vk::util::math::vec3 ejection_offset{.08f,-.04f,-.12f};
        vk::util::math::mat4 correction=vk::util::math::mat4::identity();
        float fov=1.2f,ads_fov=1.2f,near_plane=.01f,far_plane=20.f;
        float ads_zoom=0.f,ads_speed=14.f;
        float slide_offset=0.f;
        float recoil=0.f,hip_kick=0.f,ads_kick=0.f;
        float recoil_decay=15.f;
        vk::util::math::vec3 casing_impulse{1.f,1.f,0.f};
        float casing_lifetime=9.f,casing_spin=14.f;
        std::uint32_t max_casings=48;
        float flash_duration=.035f;
        vk::util::math::vec3 flash_scale{.035f,.045f,.035f};
        float impact_radius=.009f,impact_offset=.0008f,impact_lifetime=120.f;
        std::uint32_t max_impacts=192;
    };

    inline void validate(const definition&);
    inline state make_state(const definition&);
    inline void advance(state&,double,const definition&);
    inline result apply(state&,std::uint32_t,action,double,const definition&);
    inline vec3 direction(float,float);
    inline void append(world&,const std::vector<scene::mesh>&,std::span<const scene::instance>);
    inline hit trace(const world&,vec3,vec3,float);
    inline timing load_timing(const std::filesystem::path&,const std::string&,const std::string&,float,float);
}
