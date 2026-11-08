// Amalgamated original full-feature Vulkan game client.
// No references to game-local headers or compilation of clt/srv files.
// Preserves original GLFW, fastgltf, Ozz and stb code paths.

// [client.cpp]

// [clt/app/application.hpp]


// [clt/input/input.hpp]
#include <cstdint>
namespace clt::input
{
    struct frame
    {
        std::uint32_t buttons = 0;
        float mouse_x = 0;
        float mouse_y = 0;
    };
    void init();
    // GLFW key/button identifiers; query on the main thread. Unfocused input is released.
    bool key_down(int key);
    bool mouse_down(int button);
    frame poll();
    void shutdown();
} // namespace clt::input



// [srv/game/options.hpp]

#include "util/math/vector.hpp"
#include <cstdint>
#include <string>

namespace game::options
{
    enum class mode { server_only, client_only };
    enum class gbuffer_precision { unspecified, rgba8, rgba16f };
    enum class surface_format { unspecified, bgra8_srgb, rgba8_srgb };
    enum class depth_precision { unspecified, d32, d16 };
    enum class presentation { unspecified, fifo, mailbox, immediate };

    struct client_graphics
    {
        int width, height;
        std::string title, vertex_shader, fragment_shader;
        gbuffer_precision gbuffer;
        surface_format surface;
        depth_precision depth;
        presentation present;
        std::string culling_shader, lighting_shader, tonemap_shader;
        std::string fullscreen_shader, presentation_shader;
        float exposure;
    };

    struct state
    {
        options::mode mode;
        std::string host, bind_host;
        std::uint16_t port;
        double duration;
        bool headless, help;
        client_graphics graphics;
    };

    state* get();
    void init(int argc, char** argv, mode selected_mode, state configured);
    void print_help();
    void shutdown();
} // namespace game::options


#include <editor/editor.hpp>

namespace clt::application
{
    struct callbacks
    {
        // Main-thread game controls, after mouse look and movement prediction.
        void (*on_input)(const input::frame&) = nullptr;
        // Game owns spawning/property writes; editor only emits requests.
        void (*on_editor_action)(const ::editor::action&) = nullptr;
    };

    struct state
    {
        game::options::state options;
        callbacks hooks;
        bool initialized = false;
    };

    state* get();
    void set_callbacks(callbacks value);
    void init();
    int run();
    void shutdown() noexcept;
} // namespace clt::application



// [clt/weapon/weapon.hpp]

// [clt/animation/animation.hpp]


// [srv/shared/scene/scene.hpp]


// [srv/shared/scene/mesh.hpp]

#include "util/math/vector.hpp"

// [srv/shared/scene/material.hpp]

#include <array>
#include <cstdint>
#include <vector>

namespace scene
{
    // glTF metallic-roughness parameters. Texture references are imported by
    // the asset loader, independent of the authoritative scene simulation.
    struct texture
    {
        std::uint32_t id = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;
        std::vector<std::uint8_t> encoded; // Original PNG/JPEG bytes for compact network transfer.
    };

    struct material
    {
        std::array<float, 4> base_color{1, 1, 1, 1};
        std::array<float, 3> emissive{0, 0, 0};
        float metallic = 0;
        float roughness = 1;
        float occlusion = 1;
        float alpha_cutoff = 0.5f;
        std::uint32_t base_color_texture = 0;
        std::uint32_t metallic_roughness_texture = 0;
        std::uint32_t normal_texture = 0;
        std::uint32_t occlusion_texture = 0;
        std::uint32_t emissive_texture = 0;
        std::uint32_t alpha_mode = 0; // opaque, mask, blend
        bool double_sided = false;
        // 0 = ordinary material, 1 = world-space magenta/black checker.
        // Generated per fragment; no texture allocation or filtering.
        std::uint32_t procedural_pattern = 0;
    };

    inline bool same_material(const material& a, const material& b)
    {
        return a.base_color == b.base_color &&
            a.emissive == b.emissive &&
            a.metallic == b.metallic &&
            a.roughness == b.roughness &&
            a.occlusion == b.occlusion &&
            a.alpha_cutoff == b.alpha_cutoff &&
            a.base_color_texture == b.base_color_texture &&
            a.metallic_roughness_texture == b.metallic_roughness_texture &&
            a.normal_texture == b.normal_texture &&
            a.occlusion_texture == b.occlusion_texture &&
            a.emissive_texture == b.emissive_texture &&
            a.alpha_mode == b.alpha_mode &&
            a.double_sided == b.double_sided &&
            a.procedural_pattern == b.procedural_pattern;
    }
} // namespace scene



#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace scene 
{
    struct vertex {
        vk::util::math::vec3 position;
        vk::util::math::vec3 normal;
        float u = 0;
        float v = 0;
        std::array<std::uint32_t, 4> joints{};
        std::array<float, 4> weights{};
    };

    struct mesh {
        std::uint32_t id = 0;
        std::uint32_t skin_id = 0;
        std::vector<vertex> vertices;
        std::vector<std::uint32_t> indices;
        material surface;
    };

    inline bool same_mesh(const mesh& a, const mesh& b)
    {
        if (a.id != b.id || a.skin_id != b.skin_id ||
            a.vertices.size() != b.vertices.size() ||
            a.indices != b.indices || !same_material(a.surface, b.surface))
        {
            return false;
        }

        for (std::size_t i = 0; i < a.vertices.size(); ++i)
        {
            const auto& x = a.vertices[i];
            const auto& y = b.vertices[i];
            if (x.position.x != y.position.x || x.position.y != y.position.y ||
                x.position.z != y.position.z || x.normal.x != y.normal.x ||
                x.normal.y != y.normal.y || x.normal.z != y.normal.z ||
                x.u != y.u || x.v != y.v || x.joints != y.joints ||
                x.weights != y.weights)
            {
                return false;
            }
        }
        return true;
    }
}; // namespace scene


#include <array>
#include <string>
#include <limits>
#include "util/math/quaternion.hpp"

namespace scene 
{
    struct transform {
        vk::util::math::vec3 position;
        vk::util::math::vec3 scale{1, 1, 1};
        vk::util::math::quat rotation;
    };

    struct clip {
        std::string name;
        std::vector<std::uint8_t> bytes;
    };

    struct skin {
        std::uint32_t id = 0;
        std::vector<std::uint8_t> skeleton;
        std::vector<clip> clips;
        std::vector<std::array<float, 16>> inverse_bind;
    };

    struct instance {
        std::uint32_t id = 0;
        std::uint32_t mesh_id = 0;
        std::uint32_t animation = 0;
        std::uint32_t animation_tick = 0;
        std::uint32_t overlay_animation = UINT32_MAX;
        std::uint32_t overlay_root = UINT32_MAX;
        float overlay_weight = 0.0f;
        transform pose;
    };

    // Game-authored world-space AABBs, usable by physics systems without
    // repeating mesh dimensions and object positions in the client.
    struct collision_box {
        vk::util::math::vec3 minimum{};
        vk::util::math::vec3 maximum{};
    };

    struct point_light
    {
        vk::util::math::vec3 position{};
        float radius=1.0f;
        vk::util::math::vec3 color{1.0f,1.0f,1.0f};
        float strength=1.0f;
        // Server editor deletion preserves a stable light slot and its ID.
        // Disabled slots contribute zero light and can be reused by Insert.
        bool enabled=true;
    };

    struct state {
        std::vector<mesh> meshes;
        std::vector<texture> textures;
        std::vector<skin> skins;
        std::vector<instance> instances;
        std::vector<collision_box> collision_boxes;
        std::vector<point_light> lights;
    };

    inline bool same_instance(const instance& a, const instance& b)
    {
        return a.id == b.id && a.mesh_id == b.mesh_id &&
            a.animation == b.animation && a.animation_tick == b.animation_tick &&
            a.overlay_animation == b.overlay_animation &&
            a.overlay_root == b.overlay_root &&
            a.overlay_weight == b.overlay_weight &&
            a.pose.position.x == b.pose.position.x &&
            a.pose.position.y == b.pose.position.y &&
            a.pose.position.z == b.pose.position.z &&
            a.pose.scale.x == b.pose.scale.x &&
            a.pose.scale.y == b.pose.scale.y &&
            a.pose.scale.z == b.pose.scale.z &&
            a.pose.rotation.x == b.pose.rotation.x &&
            a.pose.rotation.y == b.pose.rotation.y &&
            a.pose.rotation.z == b.pose.rotation.z &&
            a.pose.rotation.w == b.pose.rotation.w;
    }
}; // namespace scene



#include <array>
#include <cstdint>
#include <vector>
#include <string_view>

namespace clt::animation
{
    using matrix = std::array<float, 16>;

    // Only called on the Vulkan render thread.
    void synchronize(const ::scene::state& scene);
    std::uint32_t joints(std::uint32_t skin_id);
    bool sample(
        std::uint32_t skin_id,
        std::uint32_t clip,
        float seconds,
        std::vector<matrix>& palette,
        std::uint32_t overlay = UINT32_MAX,
        float weight = 0.0f,
        std::uint32_t mask_root = UINT32_MAX,
        bool loop = true
    );
    float duration(std::uint32_t skin_id, std::uint32_t clip);
    // Read after sample() on the same render thread for weapon sockets/effects.
    std::uint32_t joint(std::uint32_t skin_id, std::string_view name);
    bool joint_transform(std::uint32_t skin_id, std::uint32_t index, matrix& out);
    bool offset_joint(
        std::uint32_t skin_id,
        std::uint32_t root,
        vk::util::math::vec3 model_offset,
        std::vector<matrix>& palette
    );
    void reset();
} // namespace clt::animation



// [srv/shared/weapon/gun.hpp]


// [srv/shared/weapon/weapon.hpp]


// [srv/shared/net/message.hpp]

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::message
{
    enum class type : std::uint8_t {
        hello = 1,
        welcome,
        ping,
        state,
        goodbye,
        scene_request
    };

    struct resource {
        message::type type;
        std::uint32_t sequence = 0;
        std::uint32_t token = 0;
        std::uint32_t value = 0;
    };

    inline constexpr std::size_t packet_size = 20;
    using packet = std::array<std::uint8_t, packet_size>;

    packet encode(const resource& message);
    std::optional<resource> decode(const std::uint8_t* data, std::size_t size);
    bool newer(std::uint32_t sequence, std::uint32_t previous);
} // namespace net::message



#include <spatial/index.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace weapon
{
    using vk::util::math::vec3;

    enum class action : std::uint32_t
    {
        fire = 1,
        reload = 2,
        equip = 3
    };
    enum class reload_phase : std::uint32_t
    {
        none,
        tactical,
        empty
    };
    enum class result : std::uint32_t
    {
        none,
        fired,
        cooldown,
        empty,
        reloading,
        reload_started,
        reload_unnecessary,
        equipped
    };

    struct timing
    {
        float reload_duration = 0;
        float empty_reload_duration = 0;
        float refill_time = 0;
        float empty_refill_time = 0;
    };

    struct definition
    {
        std::uint32_t id = 0;
        std::uint32_t magazine_capacity = 0;
        std::uint32_t initial_reserve = 0;
        double fire_interval = 0;
        float range = 0;
        timing reload;
    };

    inline void validate(const definition& value)
    {
        const auto& t = value.reload;
        if (!value.id || !value.magazine_capacity || !std::isfinite(value.fire_interval) ||
            value.fire_interval <= 0 || !std::isfinite(value.range) || value.range <= 0 ||
            !std::isfinite(t.reload_duration) || !std::isfinite(t.empty_reload_duration) ||
            !std::isfinite(t.refill_time) || !std::isfinite(t.empty_refill_time) ||
            t.reload_duration <= 0 || t.empty_reload_duration <= 0 || t.refill_time < 0 ||
            t.refill_time > t.reload_duration || t.empty_refill_time < 0 ||
            t.empty_refill_time > t.empty_reload_duration)
        {
            throw std::invalid_argument("Invalid weapon definition");
        }
    }

    // Definitions are stored contiguously; no registry object or virtual interface.
    using registry=std::vector<definition>;

    inline void add(registry& entries,definition value) {
        validate(value);
        if(std::any_of(entries.begin(),entries.end(),
            [&](const definition& e){return e.id==value.id;}))
            throw std::invalid_argument("Duplicate weapon ID");
        entries.push_back(std::move(value));
    }
    inline const definition* find(const registry& entries,std::uint32_t id) {
        const auto it=std::find_if(entries.begin(),entries.end(),
            [id](const definition& e){return e.id==id;});
        return it==entries.end()?nullptr:&*it;
    }

    struct state
    {
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

    inline state make_state(const definition& settings)
    {
        state value;
        value.magazine = settings.magazine_capacity;
        value.reserve = settings.initial_reserve;
        return value;
    }

    inline void advance(state& value, double now, const definition& settings)
    {
        if (value.reload == reload_phase::none)
        {
            return;
        }
        if (!value.refilled && now >= value.refill_at)
        {
            const auto amount = std::min(
                settings.magazine_capacity - std::min(value.magazine, settings.magazine_capacity),
                value.reserve
            );
            value.magazine += amount;
            value.reserve -= amount;
            value.refilled = true;
        }
        if (now >= value.reload_finishes)
        {
            value.reload = reload_phase::none;
        }
    }

    // One edge-triggered command is one semiautomatic attempt. Time comes only from the server.
    inline result apply(
        state& value, std::uint32_t sequence, action command, double now, const definition& settings
    )
    {
        advance(value, now, settings);
        if (!sequence || !net::message::newer(sequence, value.sequence))
        {
            return result::none;
        }
        value.sequence = sequence;
        if (command == action::equip)
        {
            return value.last_result = result::equipped;
        }
        if (command != action::fire && command != action::reload)
        {
            return value.last_result = result::none;
        }
        if (value.reload != reload_phase::none)
        {
            return value.last_result = result::reloading;
        }
        if (command == action::reload)
        {
            if (value.magazine == settings.magazine_capacity || !value.reserve)
            {
                return value.last_result = result::reload_unnecessary;
            }
            const auto& reload = settings.reload;
            const bool empty = !value.magazine;
            value.reload = empty ? reload_phase::empty : reload_phase::tactical;
            value.reload_started = now;
            value.reload_finishes =
                now + (empty ? reload.empty_reload_duration : reload.reload_duration);
            value.refill_at = now + (empty ? reload.empty_refill_time : reload.refill_time);
            value.refilled = false;
            return value.last_result = result::reload_started;
        }
        if (now < value.next_fire)
        {
            return value.last_result = result::cooldown;
        }
        if (!value.magazine)
        {
            return value.last_result = result::empty;
        }
        --value.magazine;
        value.next_fire = now + settings.fire_interval;
        return value.last_result = result::fired;
    }

    inline vec3 direction(float yaw, float pitch)
    {
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }

    struct triangle
    {
        vec3 a, b, c;
        std::uint32_t instance = 0, index = 0;
    };

    struct hit
    {
        bool impacted = false;
        vec3 position{}, normal{};
        std::uint32_t instance = 0, triangle = 0;
    };

    // Flat collision data and free functions, no game-owned world object API.
    struct world {
        std::vector<triangle> triangles;
        spatial::index index;
    };

    inline void append(world& data,const std::vector<scene::mesh>& meshes,
                       const std::vector<scene::instance>& instances) {
        using namespace vk::util::math;
        std::vector<spatial::bounds> boxes;
        for(const auto& instance:instances) {
            const auto mesh=std::find_if(meshes.begin(),meshes.end(),
                [&](const scene::mesh& m){return m.id==instance.mesh_id;});
            if(mesh==meshes.end())continue;
            const auto transform=[&](vec3 p) {
                return instance.pose.position+rotate(instance.pose.rotation,
                    {p.x*instance.pose.scale.x,p.y*instance.pose.scale.y,
                     p.z*instance.pose.scale.z});
            };
            for(std::size_t i=0;i+2<mesh->indices.size();i+=3) {
                const auto a=mesh->indices[i],b=mesh->indices[i+1],c=mesh->indices[i+2];
                if(a>=mesh->vertices.size()||b>=mesh->vertices.size()||
                   c>=mesh->vertices.size())continue;
                triangle t{transform(mesh->vertices[a].position),
                    transform(mesh->vertices[b].position),
                    transform(mesh->vertices[c].position),
                    instance.id,std::uint32_t(i/3)};
                data.triangles.push_back(t);
                boxes.push_back({{
                    std::min({t.a.x,t.b.x,t.c.x}),
                    std::min({t.a.y,t.b.y,t.c.y}),
                    std::min({t.a.z,t.b.z,t.c.z})},{
                    std::max({t.a.x,t.b.x,t.c.x}),
                    std::max({t.a.y,t.b.y,t.c.y}),
                    std::max({t.a.z,t.b.z,t.c.z})}});
            }
        }
        data.index.append(boxes);
    }

    inline hit trace(const world& data,vec3 origin,vec3 direction,float range) {
        using namespace vk::util::math;
        hit nearest{};
        float distance=range;
        const auto end=origin+direction*range;
        const spatial::bounds region{{
            std::min(origin.x,end.x),std::min(origin.y,end.y),std::min(origin.z,end.z)},{
            std::max(origin.x,end.x),std::max(origin.y,end.y),std::max(origin.z,end.z)}};
        data.index.query(region,[&](std::uint32_t id) {
            const auto& t=data.triangles[id];
            const auto edge1=t.b-t.a,edge2=t.c-t.a;
            const auto h=cross(direction,edge2);
            const float determinant=dot(edge1,h);
            if(std::abs(determinant)<1e-7f)return;
            const float inverse=1.f/determinant;
            const auto s=origin-t.a;
            const float u=inverse*dot(s,h);
            if(u<0||u>1)return;
            const auto q=cross(s,edge1);
            const float v=inverse*dot(direction,q);
            if(v<0||u+v>1)return;
            const float d=inverse*dot(edge2,q);
            if(d<.0001f||d>=distance)return;
            distance=d;
            auto normal=normalize(cross(edge1,edge2));
            if(dot(normal,direction)>0)normal=normal*-1.f;
            nearest={true,origin+direction*d,normal,t.instance,t.index};
        });
        return nearest;
    }
} // namespace weapon



#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vk/vk.hpp>

namespace weapon
{
    enum class motion
    {
        idle,walk,draw,reload,reload_empty,
        ads_idle,ads_in,ads_out,ads_walk,
        fire1,fire2,fire3,ads_fire1,ads_fire2,ads_fire3
    };

    struct clip
    {
        std::string name;
        float speed=1.f;
        bool loop=false;
    };

    // GLFW-compatible IDs are supplied by game code. This shared header
    // has no window/GLFW dependency and is safe for dedicated servers.
    struct bindings
    {
        int trigger_button=0,aim_button=1;
        int reload_key='R',inspect_key='M',equip_key=-1;
        bool automatic=false;
        float sensitivity=.0025f;
        float ads_sensitivity_multiplier=.60f;
    };

    // One complete description per gun; all authoritative settings and
    // client presentation remain together in game-owned weapons.cpp.
    struct gun
    {
        definition rules;
        bindings input{};
        std::filesystem::path model,casing_model;
        std::map<motion,clip> animations;
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
}



#include <cstdint>
#include <string>
#include <vector>
#include <vk/vk.hpp>

namespace clt::weapon
{
    // Shared API: the server consumes gun.rules, the client consumes its
    // model, input, camera, animation and presentation configuration.
    using motion=::weapon::motion;
    using clip=::weapon::clip;
    using bindings=::weapon::bindings;
    using definition=::weapon::gun;
    using gun=::weapon::gun;

    struct equip_binding
    {
        std::uint32_t id=0;
        int key=-1;
    };
    bindings active_bindings();
    std::vector<equip_binding> equip_bindings();

    // Game-owned declarations, registered before renderer initialization.
    void register_weapon(definition value);
    // Queued from the input thread; server validates the requested weapon ID.
    void equip(std::uint32_t id);
    // Requests a read-only magazine/reserve inspection on the render thread.
    void check_magazine();

    struct instance_data
    {
        vk::util::math::mat4 model;
        vk::util::math::mat4 normal;
    };

    struct draw
    {
        std::uint32_t mesh_id = 0;
        bool viewmodel = false;
        std::vector<instance_data> instances;
        std::vector<animation::matrix> palette;
    };

    // Input-thread producer; edge commands are retained until consumed by the render thread.
    void controls(bool trigger, bool ads, bool reload, std::uint32_t movement);
    void initialize();
    const ::scene::state& assets();
    // Placement and projection must use the same captured render-frame view.
    const std::vector<draw>& update(const vk::util::math::mat4& view);
    vk::util::math::mat4 view_projection(float aspect, const vk::util::math::mat4& view);

    struct status
    {
        std::uint32_t magazine = 0, reserve = 0;
        bool reloading = false;
        float ads = 0;
        std::string animation;
        std::size_t casings = 0, impacts = 0;
        std::uint32_t weapon_id = 0;
    };

    // Render-thread view of the predicted HUD and presentation state.
    status inspect();
    void shutdown();
} // namespace clt::weapon



// [clt/render/postprocess.hpp]
#include <vk/vk.hpp>

// [clt/view/camera.hpp]

#include <vk/vk.hpp>

namespace clt::camera
{
    struct state {
        float yaw = 0;
        float pitch = 0;
        float fov = 0;
        float sensitivity = 0;
    };

    struct frame
    {
        state pose;
        vk::util::math::mat4 view;
        vk::util::math::vec3 eye;
    };

    state* get();
    void init();
    void look(float dx, float dy);
    // Updated when the active gun changes; ADS sensitivity is interpolated
    // using the same aim fraction as the camera's zoom.
    void set_look_sensitivity(float base,float ads_multiplier);
    void set_ads_fraction(float fraction);
    state snapshot();
    // Freeze mouse look, recoil and predicted eye position for one render frame.
    frame capture();
    void kick(float radians);
    void aim(float fraction);
    vk::util::math::mat4 view_projection(float aspect);

    vk::util::math::mat4 view();
    vk::util::math::mat4 projection(float aspect);
} // namespace clt::camera


#include <cstdint>

namespace clt::postprocess
{
    void set_exposure(float exposure);
    struct targets {
        vk::Image grid{};
        vk::Buffer points{};
        vk::Image hdr{};
        vk::Image ldr{};
    };

    targets images(std::uint32_t image);
    void create();
    void synchronize_scene();
    void updated(std::uint32_t image);
    void toggle_probes();
    void release();
    void cloud(vk::CommandBuffer command,std::uint32_t image);
    void gi(vk::CommandBuffer command,std::uint32_t image);
    void lighting(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void tonemap(vk::CommandBuffer command, std::uint32_t image, const camera::frame& frame);
    void present(vk::CommandBuffer command, std::uint32_t image);
}



// [clt/network/connection.hpp]


// [srv/shared/net/animation.hpp]

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::animation
{
    // Repeated 10 Hz authoritative animation state, not skeleton poses.
    struct resource
    {
        std::uint32_t token = 0;
        std::uint32_t server_tick = 0;
        std::uint32_t instance_id = 0;
        std::uint32_t clip = 0;
        std::uint32_t start_tick = 0;
        std::uint32_t overlay = UINT32_MAX;
        std::uint32_t mask_root = UINT32_MAX;
        float blend = 0;
    };

    struct attachment_event
    {
        std::uint32_t token = 0;
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        std::uint32_t instance_id = 0;
        std::uint32_t joint = 0;
        std::uint32_t kind = 0;
    };

    using event_packet = std::array<std::uint8_t, 28>;
    event_packet encode_event(const attachment_event& event);
    std::optional<attachment_event> decode_event(const std::uint8_t* bytes, std::size_t size);

    using packet = std::array<std::uint8_t, 40>;
    packet encode(const resource& state);
    std::optional<resource> decode(const std::uint8_t* data, std::size_t bytes);
} // namespace net::animation



// [srv/shared/net/movement.hpp]


// [srv/shared/player/movement.hpp]


#include <spatial/index.hpp>

#include <cstdint>
#include <vector>

namespace player
{
    enum button : std::uint32_t {
        forward = 1u << 0,
        backward = 1u << 1,
        left = 1u << 2,
        right = 1u << 3,
        sprint = 1u << 4
    };

    inline constexpr std::uint32_t valid_buttons = 31;

    struct input {
        std::uint32_t sequence = 0;
        std::uint32_t buttons = 0;
        float yaw = 0;
    };

    struct parameters_t {
        vk::util::math::vec3 spawn;
        float walk_speed, sprint_speed, gravity, radius, height;
    };

    void configure(parameters_t values);
    const parameters_t& parameters();

    struct state {
        vk::util::math::vec3 position{};
        vk::util::math::vec3 velocity{};
        bool grounded = false;
    };

    struct box {
        vk::util::math::vec3 min;
        vk::util::math::vec3 max;
    };

    using collision_world = std::vector<box>;
    
    spatial::index build_index(const collision_world& world);
    collision_world build_colliders(const ::scene::state& scene);
    collision_world build_colliders(
        const std::vector<::scene::mesh>& meshes,
        const std::vector<::scene::instance>& instances
    );
    void step(state& player, const input& input, const collision_world& world, const spatial::index* broadphase = nullptr);
} // namespace player



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



// [srv/shared/net/editor_light.hpp]



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


#include <network/secure.hpp>

// [srv/shared/net/weapon.hpp]



#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace net::weapon
{
    using action = ::weapon::action;
    using result = ::weapon::result;
    using reload_phase = ::weapon::reload_phase;

    struct command
    {
        std::uint32_t token = 0, sequence = 0;
        action kind = action::fire;
        float yaw = 0, pitch = 0;
        std::uint32_t weapon_id = 0;
    };

    struct snapshot
    {
        std::uint32_t token = 0, sequence = 0, tick = 0;
        std::uint32_t magazine = 0, reserve = 0;
        reload_phase reload = reload_phase::none;
        result outcome = result::none;
        float reload_elapsed = 0, reload_duration = 0, refill_time = 0;
        std::uint32_t weapon_id = 0;
        std::uint32_t active_weapon_id = 0;
    };

    struct shot
    {
        snapshot state;
        ::weapon::hit impact;
    };

    using command_packet = std::array<std::uint8_t, 32>;
    using snapshot_packet = std::array<std::uint8_t, 56>;
    using shot_packet = std::array<std::uint8_t, 92>;
    command_packet encode_command(const command& value);
    std::optional<command> decode_command(const std::uint8_t* bytes, std::size_t size);
    snapshot_packet encode_snapshot(const snapshot& value);
    std::optional<snapshot> decode_snapshot(const std::uint8_t* bytes, std::size_t size);
    shot_packet encode_shot(const shot& value);
    std::optional<shot> decode_shot(const std::uint8_t* bytes, std::size_t size);
} // namespace net::weapon




#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace clt::connection
{
    struct state
    {
        std::string host;
        std::uint16_t port = 0;
        net::secure::resource socket;

        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> server_tick{0};
        std::atomic<std::uint64_t> player_snapshots{0};
        std::atomic<bool> scene_complete{false};

        std::mutex animation_mutex;
        std::unordered_map<std::uint32_t, net::animation::resource> animations;
        std::deque<net::animation::attachment_event> attachment_events;
        std::uint32_t last_attachment_sequence = 0;

        std::mutex scene_mutex;
        std::shared_ptr<const ::scene::state> scene;
        std::vector<::scene::state> pending_scene;
        std::chrono::steady_clock::time_point last_publish{};
        // Server-authoritative light changes wait until scene data is ready.
        std::array<std::uint32_t,64> light_versions{};
        std::array<std::optional<net::editor_light::resource>,64> light_pending{};

        std::mutex error_mutex;
        std::exception_ptr worker_error;

        std::mutex player_mutex;

        std::deque<::player::input> outgoing;
        std::optional<net::movement::snapshot> authority;
        std::optional<::player::parameters_t> server_parameters;

        std::mutex editor_mutex;
        std::deque<net::editor_light::resource> editor_outgoing;
        std::uint32_t editor_sequence=0;

        std::mutex weapon_mutex;
        std::deque<net::weapon::command> weapon_outgoing;
        std::deque<net::weapon::snapshot> weapon_updates;
        std::deque<net::weapon::shot> weapon_shots;
        std::uint32_t weapon_ack = 0, weapon_tick = 0;
        std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> weapon_versions;

        std::thread worker;
        bool initialized = false;
    };

    state* get();
    std::shared_ptr<const ::scene::state> snapshot();
    std::optional<net::animation::resource> animation(std::uint32_t instance_id);
    std::vector<net::animation::attachment_event> take_attachment_events();
    bool send_editor_light(net::editor_light::resource command);
    bool send_weapon(net::weapon::command command);
    std::vector<net::weapon::snapshot> take_weapon_updates();
    std::vector<net::weapon::shot> take_weapon_shots();
    void check_error();
    void send_input(const ::player::input& input);
    std::optional<net::movement::snapshot> take_authority();
    std::optional<::player::parameters_t> movement_parameters();
    void init();
    void shutdown() noexcept;
} // namespace clt::connection



// [clt/platform/window.hpp]

#include <string>

struct GLFWwindow;

namespace clt::window
{
    struct state
    {
        GLFWwindow* handle = nullptr;
        int width = 0;
        int height = 0;
        std::string title;
        bool initialized = false;
    };

    state* get();
    void init();
    bool should_close();
    bool minimized();
    void framebuffer_size(int& width, int& height);
    void poll_events();
    void shutdown() noexcept;
} // namespace clt::window




// [weapons.hpp]


#include <vector>

// Game-owned gun catalog. Both server and client consume the same complete
// profiles: the server reads authoritative rules, the client loads models,
// animation clips, controls, ADS behavior, recoil and effects.
namespace game_weapons
{
    std::vector<::weapon::gun> all();
}



// [srv/game/application.hpp]

#include <atomic>

namespace game::application
{
    struct state
    {
        std::atomic<bool> stop{false};
    };

    state* get();
} // namespace game::application



#include <network/socket.hpp>
#include <editor/editor.hpp>

#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include <limits>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>


namespace
{
    // The server's scene.light array is append-only today. Reserve a high
    // object-ID range for the editor's projected light labels, rather than
    // reusing mesh instance IDs. The renderer does not need a light mesh.
    constexpr std::uint64_t editor_light_ids=1ull<<32;

    // Define the light's editable fields ONCE, not at every placement site.
    // Every existing and newly created server light uses this schema.
    std::vector<editor::property> light_properties(
        const ::scene::point_light& light)
    {
        using kind=editor::property_kind;
        return {
            {"pos_x","Pos X",kind::real,light.position.x,-500,500,1},
            {"pos_y","Pos Y",kind::real,light.position.y,-500,500,1},
            {"pos_z","Pos Z",kind::real,light.position.z,-500,500,1},
            // Colors are displayed as percentages so arrow increments are
            // exactly 1 or 10 without jumping across normalized [0,1].
            {"color_r","Color X",kind::integer,light.color.x*100,0,100,1},
            {"color_g","Color Y",kind::integer,light.color.y*100,0,100,1},
            {"color_b","Color Z",kind::integer,light.color.z*100,0,100,1},
            {"radius","Radius",kind::real,light.radius,0.05,64,1},
            {"strength","Strength",kind::real,light.strength,0,20,1}
        };
    }

    void on_editor_action(const editor::action& action)
    {
        net::editor_light::resource command{};
        if(action.kind==editor::action_kind::insert)
        {
            if(action.type!="light")return;
            const auto frame=clt::camera::capture();
            const float yaw=frame.pose.yaw,pitch=frame.pose.pitch;
            const vk::util::math::vec3 forward{
                std::sin(yaw)*std::cos(pitch),
                std::sin(pitch),
                -std::cos(yaw)*std::cos(pitch)
            };
            command.type=net::editor_light::kind::create;
            command.index=UINT32_MAX;
            command.light.position=frame.eye+forward*2.5f;
            command.light.radius=4.0f;
            command.light.color={1.0f,0.85f,0.7f};
            command.light.strength=1.0f;
        }
        else if(action.kind==editor::action_kind::edit ||
                action.kind==editor::action_kind::remove)
        {
            if(action.object<=editor_light_ids ||
               action.object>editor_light_ids+64u)return;
            const std::size_t index=
                std::size_t(action.object-editor_light_ids-1u);
            const auto world=clt::connection::snapshot();
            if(!world || index>=world->lights.size())return;
            if(!world->lights[index].enabled)return;
            command.type=action.kind==editor::action_kind::remove ?
                net::editor_light::kind::remove:
                net::editor_light::kind::edit;
            command.index=std::uint32_t(index);
            command.light=world->lights[index];
            if(command.type==net::editor_light::kind::remove)
            {
                if(!clt::connection::send_editor_light(command))
                    std::clog<<"clt: could not queue light deletion\n";
                return;
            }
            // revision in client edit commands identifies the property:
            // the server changes that field only, then replicates the
            // entire authoritative point_light back to all clients.
            const auto& key=action.property_id;
            if(key=="pos_x")
                command.revision=0,command.light.position.x=float(action.value);
            else if(key=="pos_y")
                command.revision=1,command.light.position.y=float(action.value);
            else if(key=="pos_z")
                command.revision=2,command.light.position.z=float(action.value);
            else if(key=="color_r")
                command.revision=3,command.light.color.x=float(action.value/100.0);
            else if(key=="color_g")
                command.revision=4,command.light.color.y=float(action.value/100.0);
            else if(key=="color_b")
                command.revision=5,command.light.color.z=float(action.value/100.0);
            else if(key=="radius")
                command.revision=6,command.light.radius=float(action.value);
            else if(key=="strength")
                command.revision=7,command.light.strength=float(action.value);
            else return;
        }
        else return;

        if(!clt::connection::send_editor_light(command))
            std::clog<<"clt: could not queue editor light command\n";
    }


    void publish_editor_lights()
    {
        auto& ui=editor::instance();
        if(!ui.visible())return;

        int width=0,height=0;
        clt::window::framebuffer_size(width,height);
        if(width<=0 || height<=0)return;

        const auto world=clt::connection::snapshot();
        if(!world)
        {
            ui.set_labels({});
            return;
        }

        using namespace vk::util::math;
        const mat4 vp=clt::camera::view_projection(
            float(width)/float(height));
        std::vector<editor::screen_label> labels;
        labels.reserve(world->lights.size());
        for(std::size_t i=0;i<world->lights.size();++i)
        {
            const auto& light=world->lights[i];
            if(!light.enabled)continue;
            const vec4 clip=vp*vec4{light.position,1.0f};
            // Vulkan NDC Z is [0,1]. Projection already flips Y to
            // framebuffer-down coordinates. Never draw behind the camera.
            if(!std::isfinite(clip.w) || clip.w<=0.0001f ||
               clip.z<0.0f || clip.z>clip.w)
                continue;
            const float nx=clip.x/clip.w,ny=clip.y/clip.w;
            if(!std::isfinite(nx)||!std::isfinite(ny) ||
               nx<-1.0f||nx>1.0f||ny<-1.0f||ny>1.0f)
                continue;
            labels.push_back({
                editor_light_ids+std::uint64_t(i)+1u,
                "LIGHT",
                int(std::lround((nx*0.5f+0.5f)*float(width))),
                int(std::lround((ny*0.5f+0.5f)*float(height))),
                true
            });
        }
        ui.set_labels(std::move(labels));

        // Only refresh properties when a new authoritative snapshot arrives.
        // Otherwise arrow-key UI edits would be reset every input frame.
        static std::shared_ptr<const ::scene::state> registered;
        if(registered==world)return;
        registered=world;
        for(std::size_t i=0;i<world->lights.size();++i)
        {
            const auto& light=world->lights[i];
            if(!light.enabled)continue;
            ui.set_properties(editor_light_ids+std::uint64_t(i)+1u,
                              light_properties(light));
        }
    }

    void load_weapons()
    {
        for(auto&& gun:game_weapons::all())
            clt::weapon::register_weapon(std::move(gun));
    }

    void on_input(const clt::input::frame& frame)
    {
        // Publish the real server-streamed light positions to the existing
        // Hell2024 Icon_Light overlay. UI input and shader lights are separate.
        publish_editor_lights();
        // Clicking editor world labels must never fire the active weapon.
        if(editor::instance().captures_input())
        {
            clt::weapon::controls(false,false,false,0u);
            return;
        }
        static bool space_held=false;
        const bool space=clt::input::key_down(GLFW_KEY_SPACE);
        if(space && !space_held)clt::postprocess::toggle_probes();
        space_held=space;

        // Each gun chooses its own equip key, trigger, ADS and reload input.
        static std::array<bool,GLFW_KEY_LAST+1> equip_held{};
        for(const auto& item:clt::weapon::equip_bindings())
        {
            if(item.key<0 || item.key>GLFW_KEY_LAST)continue;
            const bool down=clt::input::key_down(item.key);
            if(down && !equip_held[item.key])
                clt::weapon::equip(item.id);
            equip_held[item.key]=down;
        }
        const auto keys=clt::weapon::active_bindings();
        static bool inspect_held=false;
        const bool inspect=clt::input::key_down(keys.inspect_key);
        if(inspect && !inspect_held)clt::weapon::check_magazine();
        inspect_held=inspect;
        clt::weapon::controls(
            clt::input::mouse_down(keys.trigger_button),
            clt::input::mouse_down(keys.aim_button),
            clt::input::key_down(keys.reload_key),frame.buttons);
    }

    void stop(int)
    {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }
}

int main(int argc, char** argv)
{
    try
    {
        game::options::state settings{};
        settings.host = "127.0.0.1";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
        settings.graphics.width = 1280;
        settings.graphics.height = 720;
        settings.graphics.title = "vulkan-game";
        settings.graphics.vertex_shader = "build/shaders/mesh.vert.spv";
        settings.graphics.fragment_shader = "build/shaders/mesh.frag.spv";
        settings.graphics.surface = game::options::surface_format::bgra8_srgb;
        settings.graphics.depth = game::options::depth_precision::d32;
        settings.graphics.present = game::options::presentation::fifo;
        settings.graphics.gbuffer = game::options::gbuffer_precision::rgba8;
        settings.graphics.culling_shader = "build/shaders/cull.comp.spv";
        settings.graphics.lighting_shader = "build/shaders/pcloud_light.comp.spv";
        settings.graphics.tonemap_shader = "build/shaders/pcloud_tone.comp.spv";
        settings.graphics.fullscreen_shader = "build/shaders/pcloud_fullscreen.vert.spv";
        settings.graphics.presentation_shader = "build/shaders/pcloud_present.frag.spv";
        settings.graphics.exposure = 1.0f;

        game::options::init(argc, argv, game::options::mode::client_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        load_weapons();
        // Host-defined editor catalog; LIGHT properties are auto-generated
        // for every light from the single schema above.
        editor::instance().set_catalog({{"light","Light","Lighting"}});
        clt::application::set_callbacks({on_input,on_editor_action});
        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        clt::application::init();
        const int result = clt::application::run();
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-client: " << e.what() << '\n';
        return 1;
    }
}



// [weapons.cpp]


// [srv/shared/weapon/metadata.hpp]



#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

namespace weapon
{
    // Consume the conversion sidecar rather than estimating animation length from gameplay time.
    // The sidecar supplies per-clip durations, frame rates and source frame bounds.
    inline timing load_timing(
        const std::filesystem::path& path,
        const std::string& reload_clip,
        const std::string& empty_reload_clip,
        float refill_frame,
        float empty_refill_frame
    )
    {
        std::ifstream input(path);
        if (!input)
        {
            throw std::runtime_error("Weapon animation metadata missing: " + path.string());
        }
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        auto clip = [&](const std::string& name, float event_frame)
        {
            std::string escaped;
            for (const char c : name)
            {
                if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos)
                {
                    escaped += '\\';
                }
                escaped += c;
            }
            const std::regex pattern("\\{[^{}]*\"name\"\\s*:\\s*\"" + escaped + "\"[^{}]*\\}");
            std::smatch match;
            if (!std::regex_search(json, match, pattern))
            {
                throw std::runtime_error("Weapon animation metadata missing clip " + name);
            }
            const std::string object = match.str();
            auto number = [&](const std::string& key)
            {
                const std::regex field(
                    "\"" + key + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"
                );
                std::smatch value;
                if (!std::regex_search(object, value, field))
                {
                    throw std::runtime_error("Weapon animation metadata missing " + key);
                }
                return std::stof(value[1].str());
            };
            const float duration = number("duration_seconds");
            const float fps = number("fps");
            if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(duration) || duration <= 0 ||
                !std::isfinite(event_frame))
            {
                throw std::runtime_error("Invalid weapon animation timing");
            }
            // The source clip start is rebased to time zero by the converter.
            return std::pair{
                duration, std::max(0.0f, event_frame - number("source_first_frame")) / fps
            };
        };
        const auto normal = clip(reload_clip, refill_frame),
                   empty = clip(empty_reload_clip, empty_refill_frame);
        return {normal.first, empty.first, normal.second, empty.second};
    }
} // namespace weapon



#include <utility>

namespace game_weapons
{
    std::vector<::weapon::gun> all()
    {
        using motion=::weapon::motion;

        ::weapon::gun glock{};
        glock.rules.id=1;
        glock.rules.magazine_capacity=15;
        glock.rules.initial_reserve=90;
        glock.rules.fire_interval=.25; 
        glock.rules.range=200;
        glock.rules.reload=weapon::load_timing(
            "assets/glock/metadata.json",
            "Glock_Reload","Glock_ReloadEmpty",20,20);

        // All controls and aiming parameters belong to this gun.
        glock.input.trigger_button=0;       // left mouse button
        glock.input.aim_button=1;           // right mouse button
        glock.input.reload_key='R';
        glock.input.inspect_key='M'; // print magazine/reserve without reloading
        glock.input.equip_key='1';
        glock.input.automatic=false; // hold to fire at the server-validated interval
        glock.input.sensitivity=.0025f;
        glock.input.ads_sensitivity_multiplier=.90f;

        glock.model="assets/glock/glock.glb";
        glock.casing_model="assets/props/casing.glb";
        glock.camera_joint="camera";
        glock.muzzle_joint="Muzzle";
        glock.ejection_joint="EjectionPort";
        glock.slide_joint="Slide";
        glock.animations={
            {motion::idle,{"Glock_Idle",1,true}},
            {motion::walk,{"Glock_Walk",1,true}},
            {motion::draw,{"Glock_Draw",1,false}},
            {motion::reload,{"Glock_Reload",1,false}},
            {motion::reload_empty,{"Glock_ReloadEmpty",1,false}},
            {motion::ads_idle,{"Glock_ADS_Idle",1,true}},
            {motion::ads_walk,{"Glock_ADS_Walk",1,true}},
            {motion::ads_in,{"Glock_ADS_In",3,false}},
            {motion::ads_out,{"Glock_ADS_Out",3,false}},
            {motion::fire1,{"Glock_Fire1",1.5f,false}},
            {motion::fire2,{"Glock_Fire2",1.5f,false}},
            {motion::fire3,{"Glock_Fire3",1.5f,false}},
            {motion::ads_fire1,{"Glock_ADS_Fire1",1.5f,false}},
            {motion::ads_fire2,{"Glock_ADS_Fire2",1.5f,false}},
            {motion::ads_fire3,{"Glock_ADS_Fire3",1.5f,false}}
        };

        glock.fov=1.1f;
        glock.ads_fov=glock.fov-.58f;
        glock.ads_zoom=.58f;
        glock.ads_speed=11.f;
        glock.slide_offset=.025f;
        glock.recoil=.022f;
        glock.hip_kick=.015f;
        glock.ads_kick=.009f;
        glock.casing_impulse={1.6f,1.5f,.35f};
        glock.casing_lifetime=9.f;
        glock.max_casings=48;
        glock.flash_duration=.035f;
        glock.flash_scale={.035f,.045f,.035f};
        glock.impact_radius=.009f;
        glock.impact_lifetime=120.f;
        glock.max_impacts=192;

        // Add additional gun structs to this list. Each can have different
        // models, sockets, animation sets, fire mode, keys and ADS sensitivity.
        return {std::move(glock)};
    }
}



// [clt/animation/animation.cpp]


#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace clt::animation
{
    namespace
    {
        struct runtime
        {
            std::uint32_t id = 0;
            ozz::animation::Skeleton skeleton;
            std::vector<ozz::animation::Animation> clips;
            ozz::animation::SamplingJob::Context context;
            std::vector<ozz::math::SoaTransform> locals;
            std::vector<ozz::math::SoaTransform> secondary;
            std::vector<ozz::math::SoaTransform> blended;
            std::vector<ozz::math::SimdFloat4> mask;
            std::vector<ozz::math::Float4x4> models;
            std::vector<matrix> inverse_bind;
        };

        std::vector<std::unique_ptr<runtime>>& skins()
        {
            static std::vector<std::unique_ptr<runtime>> storage;
            return storage;
        }

        template<class T>
        void read(const std::vector<std::uint8_t>& bytes, T& output)
        {
            if (bytes.empty()) throw std::runtime_error("Empty Ozz archive");
            ozz::io::MemoryStream stream;
            if (stream.Write(bytes.data(), bytes.size()) != bytes.size() ||
                stream.Seek(0, ozz::io::Stream::kSet))
                throw std::runtime_error("Invalid Ozz archive stream");
            ozz::io::IArchive archive(&stream);
            if (!archive.TestTag<T>())
                throw std::runtime_error("Ozz archive tag mismatch");
            archive >> output;
        }

        runtime* find(std::uint32_t id)
        {
            for (auto& item : skins())
                if (item->id == id) return item.get();
            return nullptr;
        }

        matrix multiply(const matrix& lhs, const matrix& rhs)
        {
            matrix result{};
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    for (int k = 0; k < 4; ++k)
                        result[column * 4 + row] +=
                            lhs[k * 4 + row] * rhs[column * 4 + k];
            return result;
        }
    } // namespace

    void synchronize(const ::scene::state& scene)
    {
        for (const auto& incoming : scene.skins)
        {
            if (find(incoming.id)) continue;
            auto item = std::make_unique<runtime>();
            item->id = incoming.id;
            read(incoming.skeleton, item->skeleton);
            if (item->skeleton.num_joints() == 0 ||
                incoming.inverse_bind.size() != std::size_t(item->skeleton.num_joints()))
                throw std::runtime_error("Invalid streamed skin joint count");
            item->inverse_bind = incoming.inverse_bind;
            item->clips.resize(incoming.clips.size());
            for (std::size_t i = 0; i < incoming.clips.size(); ++i)
            {
                read(incoming.clips[i].bytes, item->clips[i]);
                if (item->clips[i].num_tracks() != item->skeleton.num_joints())
                    throw std::runtime_error("Ozz clip does not match skin skeleton");
            }
            item->context.Resize(item->skeleton.num_joints());
            item->locals.resize(item->skeleton.num_soa_joints());
            item->secondary.resize(item->skeleton.num_soa_joints());
            item->blended.resize(item->skeleton.num_soa_joints());
            item->mask.resize(item->skeleton.num_soa_joints());
            item->models.resize(item->skeleton.num_joints());
            skins().push_back(std::move(item));
        }
    }

    std::uint32_t joints(std::uint32_t skin_id)
    {
        const auto* item = find(skin_id);
        return item ? std::uint32_t(item->skeleton.num_joints()) : 0;
    }

    bool sample(
        std::uint32_t skin_id,
        std::uint32_t clip,
        float seconds,
        std::vector<matrix>& palette,
        std::uint32_t overlay,
        float weight,
        std::uint32_t mask_root,
        bool loop
    )
    {
        auto* item = find(skin_id);
        if (!item || !std::isfinite(seconds) || !std::isfinite(weight)) return false;

        auto evaluate = [&](std::uint32_t index,
                            std::vector<ozz::math::SoaTransform>& output)
        {
            if (item->clips.empty())
            {
                std::copy(item->skeleton.joint_rest_poses().begin(),
                          item->skeleton.joint_rest_poses().end(), output.begin());
                return true;
            }
            if (index >= item->clips.size()) return false;
            const auto& animation = item->clips[index];
            if (animation.duration() <= 0) return false;
            float t = loop ? std::fmod(seconds, animation.duration())
                           : std::clamp(seconds, 0.0f, animation.duration());
            if (t < 0) t += animation.duration();
            ozz::animation::SamplingJob job;
            job.animation = &animation;
            job.context = &item->context;
            job.ratio = t / animation.duration();
            job.output = ozz::make_span(output);
            return job.Run();
        };

        if (!evaluate(clip, item->locals)) return false;
        bool blending = overlay != UINT32_MAX && weight > 0.0f;
        if (blending)
        {
            if (!evaluate(overlay, item->secondary)) return false;
            weight = std::clamp(weight, 0.0f, 1.0f);
            const auto parents = item->skeleton.joint_parents();

            if (mask_root != UINT32_MAX)
            {
                if (mask_root >= std::uint32_t(item->skeleton.num_joints()))
                    return false;
                for (std::size_t i = 0; i < item->mask.size(); ++i)
                {
                    float lane[4]{};
                    for (std::size_t j = 0; j < 4; ++j)
                    {
                        auto bone = int(i * 4 + j);
                        while (bone >= 0 && bone < int(parents.size()))
                        {
                            if (std::uint32_t(bone) == mask_root)
                            {
                                lane[j] = 1.0f;
                                break;
                            }
                            bone = parents[bone];
                        }
                    }
                    item->mask[i] = ozz::math::simd_float4::Load(
                        lane[0], lane[1], lane[2], lane[3]);
                }
            }

            std::array<ozz::animation::BlendingJob::Layer, 2> layers{};
            layers[0].weight = std::max(0.0001f, 1.0f - weight);
            layers[0].transform = ozz::make_span(item->locals);
            layers[1].weight = weight;
            layers[1].transform = ozz::make_span(item->secondary);
            if (mask_root != UINT32_MAX) layers[1].joint_weights = ozz::make_span(item->mask);

            ozz::animation::BlendingJob job;
            job.threshold = 0.00001f;
            job.layers = ozz::make_span(layers);
            job.rest_pose = item->skeleton.joint_rest_poses();
            job.output = ozz::make_span(item->blended);
            if (!job.Run()) return false;
        }

        ozz::animation::LocalToModelJob job;
        job.skeleton = &item->skeleton;
        job.input = blending ? ozz::make_span(item->blended) :
            ozz::make_span(item->locals);
        job.output = ozz::make_span(item->models);
        if (!job.Run()) return false;

        palette.resize(item->models.size());
        for (std::size_t i = 0; i < item->models.size(); ++i)
        {
            matrix world{};
            for (int c = 0; c < 4; ++c)
                ozz::math::StorePtrU(item->models[i].cols[c], world.data() + c * 4);
            palette[i] = multiply(world, item->inverse_bind[i]);
        }
        return true;
    }

    float duration(std::uint32_t skin_id, std::uint32_t clip)
    {
        const auto* skin = find(skin_id);
        return skin && clip < skin->clips.size() ? skin->clips[clip].duration() : 0.f;
    }

    std::uint32_t joint(std::uint32_t skin_id, std::string_view name)
    {
        const auto* skin = find(skin_id);
        if (!skin) return UINT32_MAX;
        const auto names = skin->skeleton.joint_names();
        for (std::uint32_t i = 0; i < names.size(); ++i)
            if (name == names[i]) return i;
        return UINT32_MAX;
    }

    bool joint_transform(std::uint32_t skin_id, std::uint32_t index, matrix& out)
    {
        const auto* skin = find(skin_id);
        if (!skin || index >= skin->models.size()) return false;
        for (int c = 0; c < 4; ++c)
            ozz::math::StorePtrU(skin->models[index].cols[c], out.data() + c * 4);
        return true;
    }

    bool offset_joint(
        std::uint32_t skin_id,
        std::uint32_t root,
        vk::util::math::vec3 offset,
        std::vector<matrix>& palette
    )
    {
        auto* skin = find(skin_id);
        if (!skin || root >= skin->models.size() || palette.size() != skin->models.size())
        {
            return false;
        }
        const auto parents = skin->skeleton.joint_parents();
        for (std::size_t i = 0; i < skin->models.size(); ++i)
        {
            int ancestor = int(i);
            while (ancestor >= 0 && std::uint32_t(ancestor) != root)
            {
                ancestor = parents[ancestor];
            }
            if (ancestor < 0)
            {
                continue;
            }
            auto& model = skin->models[i];
            model.cols[3] =
                model.cols[3] + ozz::math::simd_float4::Load(offset.x, offset.y, offset.z, 0);
            matrix world{};
            for (int c = 0; c < 4; ++c)
            {
                ozz::math::StorePtrU(model.cols[c], world.data() + c * 4);
            }
            palette[i] = multiply(world, skin->inverse_bind[i]);
        }
        return true;
    }

    void reset()
    {
        skins().clear();
    }
} // namespace clt::animation



// [clt/app/application.cpp]





// [clt/platform/vulkan_loader.hpp]

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace clt::vulkan_loader
{
    struct state
    {
        PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
        void* library = nullptr;
        bool initialized = false;
    };

    state* get();
    PFN_vkGetInstanceProcAddr init();
    void shutdown() noexcept;
} // namespace clt::vulkan_loader




// [clt/player/prediction.hpp]



#include <cstdint>

namespace clt::prediction
{
    void init();
    void controls(std::uint32_t buttons, float yaw);
    vk::util::math::vec3 position();
    std::uint64_t ticks();
    float correction();
    void shutdown();
} // namespace clt::prediction




// [clt/render/renderer.hpp]
#include <render/frame.hpp>
#include <render/sequence.hpp>
#include <gpu/timestamps.hpp>
#include <array>
#include <cstdint>

namespace clt::renderer {
    struct state {
        ::render::frame_scheduler frames;
        ::render::sequence passes;
        gpu::timestamps timings;
        std::array<double,6> gpu_ms{};
        std::uint32_t profile_frames=0;
        bool timestamps_supported=false;
        bool initialized=false;
    };
    state* get();
    void init(const char* vertex_path,const char* fragment_path);
    void resize();
    void draw_frame();
    void shutdown() noexcept;
}




#include <editor/editor.hpp>
#include <GLFW/glfw3.h>
#include <array>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

namespace clt::application
{
    namespace
    {
        // Character events avoid keyboard-layout-specific physical slash keys:
        // German layouts must toggle with the typed '?' just like US layouts.
        std::atomic<bool> typed_question{false};
        void editor_character(GLFWwindow*,unsigned int codepoint)
        {
            if(codepoint==static_cast<unsigned int>('?'))
                typed_question.store(true,std::memory_order_relaxed);
        }
    }

    state* get()
    {
        static state data;
        return &data;
    }

    void set_callbacks(callbacks value)
    {
        if (get()->initialized)
        {
            throw std::logic_error("Register game callbacks before client initialization");
        }
        get()->hooks = value;
    }

    void init()
    {
        if (get()->initialized)
        {
            throw std::runtime_error("clt::application already initialized");
        }

        get()->options = *game::options::get();

        connection::get()->host = get()->options.host;
        connection::get()->port = get()->options.port;
        connection::init();
        get()->initialized = true;
    }

    int run()
    {
        if (!get()->initialized)
        {
            throw std::runtime_error("clt::application is not initialized");
        }

        using clock = std::chrono::steady_clock;
        const auto started = clock::now();

        auto running = [&]
        {
            if (game::application::get()->stop.load(std::memory_order_relaxed))
            {
                return false;
            }

            if (get()->options.duration == 0)
            {
                return true;
            }

            const auto elapsed = std::chrono::duration<double>(clock::now() - started);

            return elapsed.count() < get()->options.duration;
        };

        if (get()->options.headless)
        {
            while (running())
            {
                connection::check_error();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            connection::check_error();

            const bool connected = connection::get()->connected.load(std::memory_order_relaxed);
            const auto scene = connection::snapshot();

            return connected && scene ? 0 : 1;
        }

        window::init();
        glfwSetCharCallback(window::get()->handle,editor_character);

        input::init();
        camera::init();
        prediction::init();

        postprocess::set_exposure(get()->options.graphics.exposure);
        renderer::init(get()->options.graphics.vertex_shader.c_str(),
                       get()->options.graphics.fragment_shader.c_str());

        std::atomic<bool> render_stop{false};
        std::exception_ptr render_error;
        std::thread render_thread([&]
        {
            try
            {
                while (!render_stop.load(std::memory_order_relaxed))
                {
                    if (!window::minimized())
                    {
                        renderer::draw_frame();
                    }
                    else
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            }
            catch (...)
            {
                render_error = std::current_exception();
                render_stop.store(true, std::memory_order_relaxed);
            }
        });

        try
        {
            while (running() && !window::should_close() &&
                   !render_stop.load(std::memory_order_relaxed))
            {
                connection::check_error();
                window::poll_events();

                const auto frame = input::poll();

                // GLFW is only the current host input adapter. VulkanEditor
                // consumes platform-independent key press events and mouse
                // pixel coordinates, leaving all editor hotkeys in one module.
                static std::array<bool,8> held{};
                const auto pressed=[](int key,std::size_t index)
                {
                    const bool down=input::key_down(key);
                    const bool edge=down && !held[index];
                    held[index]=down;
                    return edge;
                };
                editor::input events{};
                events.question=typed_question.exchange(false,
                    std::memory_order_relaxed);
                events.tab=pressed(GLFW_KEY_TAB,0);
                const bool main_enter=pressed(GLFW_KEY_ENTER,1);
                const bool keypad_enter=pressed(GLFW_KEY_KP_ENTER,2);
                events.enter=main_enter || keypad_enter;
                events.escape=pressed(GLFW_KEY_ESCAPE,3);
                events.up=pressed(GLFW_KEY_UP,4);
                events.down=pressed(GLFW_KEY_DOWN,5);
                events.left=pressed(GLFW_KEY_LEFT,6);
                events.right=pressed(GLFW_KEY_RIGHT,7);
                events.shift=input::key_down(GLFW_KEY_LEFT_SHIFT) ||
                             input::key_down(GLFW_KEY_RIGHT_SHIFT);
                static bool mouse_held=false;
                const bool mouse_down=input::mouse_down(GLFW_MOUSE_BUTTON_LEFT);
                events.click=mouse_down && !mouse_held;
                events.mouse_down=mouse_down;
                mouse_held=mouse_down;

                auto* handle=window::get()->handle;
                double cursor_x=0,cursor_y=0;
                int window_width=0,window_height=0;
                int display_width=0,display_height=0;
                glfwGetCursorPos(handle,&cursor_x,&cursor_y);
                glfwGetWindowSize(handle,&window_width,&window_height);
                window::framebuffer_size(display_width,display_height);
                events.viewport_width=display_width;
                events.viewport_height=display_height;
                events.mouse_x=int(cursor_x*double(display_width)/
                    double(std::max(1,window_width)));
                events.mouse_y=int(cursor_y*double(display_height)/
                    double(std::max(1,window_height)));
                editor::instance().handle(events);

                if(events.tab)
                {
                    const bool free_cursor=editor::instance().cursor_free();
                    glfwSetInputMode(handle,GLFW_CURSOR,free_cursor?
                        GLFW_CURSOR_NORMAL:GLFW_CURSOR_DISABLED);
                    if(glfwRawMouseMotionSupported())
                        glfwSetInputMode(handle,GLFW_RAW_MOUSE_MOTION,
                            free_cursor?GLFW_FALSE:GLFW_TRUE);
                }

                const bool captured=editor::instance().captures_input();
                if(!captured && !events.tab)
                    camera::look(frame.mouse_x,frame.mouse_y);
                prediction::controls(captured?0u:frame.buttons,
                                     camera::snapshot().yaw);
                if(get()->hooks.on_input)
                    get()->hooks.on_input(frame);
                for(const auto& action:editor::instance().take_actions())
                    if(get()->hooks.on_editor_action)
                        get()->hooks.on_editor_action(action);

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        catch (...)
        {
            render_stop.store(true, std::memory_order_relaxed);
            render_thread.join();
            throw;
        }

        render_stop.store(true, std::memory_order_relaxed);
        render_thread.join();

        if (render_error)
        {
            std::rethrow_exception(render_error);
        }

        return 0;
    }

    void shutdown() noexcept
    {
        prediction::shutdown();
        renderer::shutdown();

        input::shutdown();
        window::shutdown();

        vulkan_loader::shutdown();
        connection::shutdown();
        get()->initialized = false;
    }
} // namespace clt::application



// [clt/input/input.cpp]



#include <GLFW/glfw3.h>

namespace clt::input
{
    namespace
    {
        double last_x = 0;
        double last_y = 0;
    }

    void init()
    {
        auto* window = clt::window::get()->handle;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        glfwGetCursorPos(window, &last_x, &last_y);
    }

    bool key_down(int key)
    {
        auto* window = clt::window::get()->handle;
        return window && key >= GLFW_KEY_SPACE && key <= GLFW_KEY_LAST &&
               glfwGetWindowAttrib(window, GLFW_FOCUSED) && glfwGetKey(window, key) == GLFW_PRESS;
    }

    bool mouse_down(int button)
    {
        auto* window = clt::window::get()->handle;
        return window && button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST &&
               glfwGetWindowAttrib(window, GLFW_FOCUSED) &&
               glfwGetMouseButton(window, button) == GLFW_PRESS;
    }

    frame poll()
    {
        auto* window = clt::window::get()->handle;
        double x, y;
        glfwGetCursorPos(window, &x, &y);
        frame result{};
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED))
        {
            result.mouse_x = float(x - last_x);
            result.mouse_y = float(y - last_y);
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                result.buttons |= ::player::forward;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                result.buttons |= ::player::backward;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                result.buttons |= ::player::left;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                result.buttons |= ::player::right;
        }
        last_x = x;
        last_y = y;
        return result;
    }

    void shutdown()
    {
        if (clt::window::get()->handle)
            glfwSetInputMode(clt::window::get()->handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
} // namespace clt::input



// [clt/network/connection.cpp]


// [srv/shared/net/limits.hpp]
#include <cstddef>
#include <cstdint>

namespace net::limits
{
    inline constexpr std::uint32_t tick_hz = 60;
    inline constexpr std::uint32_t max_clients = 64;
    inline constexpr std::uint32_t timeout_seconds = 5;
    inline constexpr std::uint32_t receive_packets = 256;
    inline constexpr std::uint32_t receive_ms = 2;
    inline constexpr std::uint32_t inputs_per_tick = 4;
    inline constexpr std::uint32_t animation_period = 6;
    inline constexpr std::uint32_t animations_per_tick = 64;
    inline constexpr std::uint32_t generation_slack_ms = 2;
    inline constexpr std::uint32_t generation_commit_ticks = 8;
    inline constexpr std::uint32_t connection_timeout_ms = 3000;
    inline constexpr std::uint32_t retry_ms = 250;
    inline constexpr std::uint32_t retry_max_ms = 1000;
    inline constexpr std::uint32_t heartbeat_ms = 50;
    inline constexpr std::uint32_t scene_request_ms = 100;
    inline constexpr std::uint32_t movement_ms = 16;
}


#include <iostream>

#include <network/endpoint.hpp>


#include <network/secure.hpp>

// [srv/shared/net/secure_config.hpp]
#include <network/secure.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace game::secure_config {
inline std::string env(const char* key) {
    const char* value=std::getenv(key);
    return value && *value ? value : "";
}
inline std::string storage_directory() {
    namespace fs=std::filesystem;
    // Keep the private server identity and the client's trust pins outside Git.
    const auto xdg=env("XDG_CONFIG_HOME");
    if(!xdg.empty())return (fs::path(xdg)/"vulkan-game"/"network").string();
    const auto local=env("LOCALAPPDATA");
    if(!local.empty())return (fs::path(local)/"vulkan-game"/"network").string();
    const auto appdata=env("APPDATA");
    if(!appdata.empty())return (fs::path(appdata)/"vulkan-game"/"network").string();
    const auto home=env("HOME");
    if(!home.empty())return (fs::path(home)/".config"/"vulkan-game"/"network").string();
    throw std::runtime_error("Cannot locate per-user DTLS storage; set XDG_CONFIG_HOME or HOME");
}
inline net::secure::settings load(net::secure::role side,
                                  const std::string& server_host={},
                                  std::uint16_t server_port=0) {
    net::secure::settings s;
    s.storage_directory=storage_directory();
    if(side==net::secure::role::client) {
        if(server_host.empty() || !server_port)
            throw std::invalid_argument("Client needs --host and --port");
        s.max_peers=1;
        s.trust_identity=server_host+":"+std::to_string(server_port);
    }
    return s;
}
}


#include <network/session.hpp>


// [srv/shared/net/scene_stream.hpp]

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net::scene_stream {
inline constexpr std::size_t max_scene_bytes=75*1024*1024;
// Application-owned envelope for one complete scene batch. Fragmentation,
// retransmission and congestion control are implemented by net::session.
struct batch_payload {
    std::uint32_t token=0;
    std::uint32_t batch=0;
    bool final=false;
    std::vector<std::uint8_t> scene;
};
std::vector<std::uint8_t> serialize(const ::scene::state& scene);
bool deserialize(const std::vector<std::uint8_t>& bytes,::scene::state& scene);
std::uint32_t checksum(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_batch(const std::vector<std::uint8_t>& bytes,
    std::uint32_t token,std::uint32_t batch,bool final);
bool decode_batch(const std::vector<std::uint8_t>& bytes,batch_payload& result);
}



#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <iterator>
#include <vector>

#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>

namespace clt::connection
{
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    namespace
    {
        void run(const std::string& host, std::uint16_t port);
    }

    state* get()
    {
        static state data;
        return &data;
    }

    std::shared_ptr<const scene::state> snapshot()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.scene_mutex);
        // Received light changes are applied together with ordinary scene
        // batches to a new immutable snapshot, never to a renderer-owned one.
        std::size_t known_lights=data.scene?data.scene->lights.size():0;
        for(const auto& batch:data.pending_scene)
            known_lights+=batch.lights.size();
        bool applicable=false;
        for(std::size_t i=0;i<data.light_pending.size() && i<=known_lights;++i)
            if(data.light_pending[i]){applicable=true;break;}
        if(data.pending_scene.empty() && (!applicable || !data.scene))
            return data.scene;

        const auto now = clock::now();
        // Keep received batches as independent chunks until publication.
        // Coalesce bursts without mutating snapshots held by render/physics.
        if (data.scene && !data.scene_complete.load(std::memory_order_relaxed) &&
            now - data.last_publish < 16ms)
        {
            return data.scene;
        }

        auto combined = std::make_shared<::scene::state>();
        std::size_t skins = data.scene ? data.scene->skins.size() : 0;
        std::size_t textures = data.scene ? data.scene->textures.size() : 0;
        std::size_t meshes = data.scene ? data.scene->meshes.size() : 0;
        std::size_t instances = data.scene ? data.scene->instances.size() : 0;
        std::size_t boxes = data.scene ? data.scene->collision_boxes.size() : 0;
        std::size_t lights = data.scene ? data.scene->lights.size() : 0;
        for (const auto& patch : data.pending_scene)
        {
            skins += patch.skins.size();
            textures += patch.textures.size();
            meshes += patch.meshes.size();
            instances += patch.instances.size();
            boxes += patch.collision_boxes.size();
            lights += patch.lights.size();
        }
        combined->skins.reserve(skins);
        combined->textures.reserve(textures);
        combined->meshes.reserve(meshes);
        combined->instances.reserve(instances);
        combined->collision_boxes.reserve(boxes);
        combined->lights.reserve(lights);
        if (data.scene)
        {
            combined->skins = data.scene->skins;
            combined->textures = data.scene->textures;
            combined->meshes = data.scene->meshes;
            combined->instances = data.scene->instances;
            combined->collision_boxes = data.scene->collision_boxes;
            combined->lights = data.scene->lights;
        }
        for (auto& patch : data.pending_scene)
        {
            combined->skins.insert(
                combined->skins.end(),
                std::make_move_iterator(patch.skins.begin()),
                std::make_move_iterator(patch.skins.end())
            );
            combined->textures.insert(
                combined->textures.end(),
                std::make_move_iterator(patch.textures.begin()),
                std::make_move_iterator(patch.textures.end())
            );
            combined->meshes.insert(
                combined->meshes.end(),
                std::make_move_iterator(patch.meshes.begin()),
                std::make_move_iterator(patch.meshes.end())
            );
            combined->instances.insert(
                combined->instances.end(),
                std::make_move_iterator(patch.instances.begin()),
                std::make_move_iterator(patch.instances.end())
            );
            combined->collision_boxes.insert(
                combined->collision_boxes.end(),
                std::make_move_iterator(patch.collision_boxes.begin()),
                std::make_move_iterator(patch.collision_boxes.end())
            );
            combined->lights.insert(
                combined->lights.end(),
                std::make_move_iterator(patch.lights.begin()),
                std::make_move_iterator(patch.lights.end())
            );
        }
        bool light_changed=false;
        for(std::size_t i=0;i<data.light_pending.size();++i)
        {
            auto& pending=data.light_pending[i];
            if(!pending || i>combined->lights.size())continue;
            const auto& value=pending->light;
            if(i==combined->lights.size())
            {
                combined->lights.push_back(value);
                light_changed=true;
            }
            else
            {
                const auto& old=combined->lights[i];
                if(old.position.x!=value.position.x ||
                   old.position.y!=value.position.y ||
                   old.position.z!=value.position.z ||
                   old.radius!=value.radius ||
                   old.color.x!=value.color.x ||
                   old.color.y!=value.color.y ||
                   old.color.z!=value.color.z ||
                   old.strength!=value.strength ||
                   old.enabled!=value.enabled)
                {
                    combined->lights[i]=value;
                    light_changed=true;
                }
            }
            pending.reset();
        }
        if(data.pending_scene.empty() && !light_changed)
            return data.scene;
        data.scene = std::move(combined);
        data.pending_scene.clear();
        data.last_publish = now;
        return data.scene;
    }

    std::optional<net::animation::resource> animation(std::uint32_t instance_id)
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.animation_mutex);
        const auto it = data.animations.find(instance_id);
        if (it == data.animations.end()) return std::nullopt;
        return it->second;
    }

    std::vector<net::animation::attachment_event> take_attachment_events()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.animation_mutex);
        std::vector<net::animation::attachment_event> events(
            data.attachment_events.begin(), data.attachment_events.end());
        data.attachment_events.clear();
        return events;
    }

    bool send_editor_light(net::editor_light::resource command)
    {
        if((command.type!=net::editor_light::kind::create &&
            command.type!=net::editor_light::kind::edit &&
            command.type!=net::editor_light::kind::remove) ||
           !net::editor_light::valid(command.light) ||
           !get()->connected.load(std::memory_order_relaxed))
            return false;
        auto& data=*get();
        std::lock_guard<std::mutex> lock(data.editor_mutex);
        if(data.editor_outgoing.size()>=64)return false;
        command.sequence=++data.editor_sequence;
        if(!command.sequence)command.sequence=++data.editor_sequence;
        command.index=command.type==net::editor_light::kind::create ?
            UINT32_MAX:command.index;
        data.editor_outgoing.push_back(command);
        return true;
    }

    bool send_weapon(net::weapon::command command)
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.weapon_mutex);
        if (data.weapon_outgoing.size() < 64)
        {
            data.weapon_outgoing.push_back(command);
            return true;
        }
        return false;
    }

    std::vector<net::weapon::snapshot> take_weapon_updates()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.weapon_mutex);
        std::vector<net::weapon::snapshot> result(
            data.weapon_updates.begin(), data.weapon_updates.end()
        );
        data.weapon_updates.clear();
        return result;
    }

    std::vector<net::weapon::shot> take_weapon_shots()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.weapon_mutex);
        std::vector<net::weapon::shot> result(data.weapon_shots.begin(), data.weapon_shots.end());
        data.weapon_shots.clear();
        return result;
    }

    void check_error()
    {
        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(get()->error_mutex);
            error = get()->worker_error;
        }
        if (error)
        {
            std::rethrow_exception(error);
        }
    }

    void send_input(const ::player::input& input)
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.player_mutex);

        if (data.outgoing.size() >= 128)
        {
            data.outgoing.pop_front();
        }

        data.outgoing.push_back(input);
    }

    std::optional<net::movement::snapshot> take_authority()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.player_mutex);

        auto result = data.authority;
        data.authority.reset();
        return result;
    }

    std::optional<::player::parameters_t> movement_parameters()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.player_mutex);
        return data.server_parameters;
    }

    void init()
    {
        if (get()->initialized || get()->worker.joinable())
        {
            throw std::runtime_error("clt::connection already initialized");
        }
        
        {
            std::lock_guard<std::mutex> lock(get()->player_mutex);
            get()->outgoing.clear();
            get()->authority.reset();
            get()->server_parameters.reset();
        }
        {
            std::lock_guard<std::mutex> lock(get()->editor_mutex);
            get()->editor_outgoing.clear();
            get()->editor_sequence=0;
        }

        {
            std::lock_guard<std::mutex> lock(get()->scene_mutex);
            get()->scene.reset();
            get()->pending_scene.clear();
            get()->last_publish = {};
            get()->light_versions.fill(0);
            get()->light_pending.fill(std::nullopt);
        }

        {
            std::lock_guard<std::mutex> lock(get()->error_mutex);
            get()->worker_error = {};
        }

        get()->stop.store(false, std::memory_order_relaxed);
        get()->connected.store(false, std::memory_order_relaxed);
        get()->server_tick.store(0, std::memory_order_relaxed);
        get()->player_snapshots.store(0, std::memory_order_relaxed);
        get()->scene_complete.store(false, std::memory_order_relaxed);
        get()->worker = std::thread(run, get()->host, get()->port);
        get()->initialized = true;
    }

    void shutdown() noexcept
    {
        get()->stop.store(true, std::memory_order_relaxed);

        if (get()->worker.joinable())
        {
            get()->worker.join();
        }
        
        {
            std::lock_guard<std::mutex> lock(get()->player_mutex);
            get()->outgoing.clear();
            get()->authority.reset();
        }
        {
            std::lock_guard<std::mutex> lock(get()->editor_mutex);
            get()->editor_outgoing.clear();
            get()->editor_sequence=0;
        }

        {
            std::lock_guard<std::mutex> lock(get()->scene_mutex);
            get()->scene.reset();
            get()->pending_scene.clear();
            get()->last_publish = {};
            get()->light_versions.fill(0);
            get()->light_pending.fill(std::nullopt);
        }

        net::secure::destroy(get()->socket);
        {
            std::lock_guard<std::mutex> lock(get()->error_mutex);
            get()->worker_error = {};
        }
        get()->connected.store(false, std::memory_order_relaxed);
        get()->server_tick.store(0, std::memory_order_relaxed);
        get()->scene_complete.store(false, std::memory_order_relaxed);
        get()->initialized = false;
    }

    namespace
    {
        void run(const std::string& host, std::uint16_t port)
        {
            auto& data = *get();

            try
            {
                const auto server=net::endpoint::resolve(host,port);
                data.socket=net::secure::create(
                    net::secure::role::client,
                    game::secure_config::load(net::secure::role::client,host,port),0,
                    server.address.ss_family==AF_INET6?"::":"0.0.0.0");
                net::secure::connect(data.socket,server);
                net::session::settings transport_settings;
                transport_settings.channel.max_message=net::scene_stream::max_scene_bytes+20;
                transport_settings.channel.max_queued=2*(net::scene_stream::max_scene_bytes+20);
                transport_settings.channel.window=512;
                transport_settings.max_delivered_bytes=2*(net::scene_stream::max_scene_bytes+20);
                transport_settings.max_peers=1;
                transport_settings.allow_unknown_peers=false;
                net::session transport(transport_settings);
                transport.open(server);
                std::uint32_t token = std::random_device{}() | 1u;

                std::uint32_t sequence = 0;
                std::uint32_t outstanding = 0;
                std::uint32_t last_accepted = 0;
                bool waiting = false;
                
                std::uint32_t last_player_tick = 0;
                std::deque<::player::input> unacknowledged;

                std::uint32_t scene_batch=0;
                clock::time_point stream_started{};
                std::size_t transferred_bytes = 0;
                bool scene_ready = false;

                auto next_send = clock::now();
                auto next_scene_send = next_send;
                auto next_move_send = next_send;
                auto next_weapon_send = next_send;
                auto next_editor_send = next_send;
                std::uint32_t editor_retry_sequence=0;
                std::uint32_t editor_retry_attempts=0;
                auto last_received = next_send;
                auto retry = std::chrono::milliseconds(net::limits::retry_ms);
                std::array<std::uint8_t,1473> buffer{};

                auto clear_scene = [&]
                {
                    {
                        std::lock_guard<std::mutex> lock(data.weapon_mutex);
                        data.weapon_outgoing.clear();
                        data.weapon_updates.clear();
                        data.weapon_shots.clear();
                        data.weapon_ack = data.weapon_tick = 0;
                        data.weapon_versions.clear();
                    }
                    {
                        std::lock_guard<std::mutex> lock(data.editor_mutex);
                        // Keep monotonically increasing editor command IDs
                        // across scene retries with the same DTLS session token.
                        data.editor_outgoing.clear();
                    }
                    transport.clear();
                    transport.open(server);
                    stream_started = {};
                    transferred_bytes = 0;
                    {
                        std::lock_guard<std::mutex> lock(data.player_mutex);
                        data.server_parameters.reset();
                    }
                    scene_batch = 0;
                    scene_ready = false;
                    data.scene_complete.store(false, std::memory_order_relaxed);

                    std::lock_guard<std::mutex> lock(data.scene_mutex);
                    data.scene.reset();
                    data.pending_scene.clear();
                    data.last_publish = {};
                    data.light_versions.fill(0);
                    data.light_pending.fill(std::nullopt);
                    {
                        std::lock_guard<std::mutex> animation_lock(data.animation_mutex);
                        data.animations.clear();
                        data.attachment_events.clear();
                        data.last_attachment_sequence = 0;
                    }
                };

                auto send = [&](net::message::type type)
                {
                    const auto packet = net::message::encode({type, ++sequence, token, 0});
                    net::secure::send(data.socket, server, packet.data(), packet.size());
                    outstanding = sequence;
                };

                while (!data.stop.load(std::memory_order_relaxed))
                {
                    const auto now = clock::now();

                    for (int budget = 0; budget < 512; ++budget)
                    {
                        net::endpoint::resource peer;
                        const auto size = net::secure::receive(data.socket, peer, buffer.data(), buffer.size());

                        if (!size)
                        {
                            break;
                        }

                        if(const auto update=net::editor_light::decode(
                            buffer.data(),*size))
                        {
                            if(net::endpoint::equal(peer,server) &&
                               update->token==token &&
                               data.connected.load(std::memory_order_relaxed) &&
                               (update->type==net::editor_light::kind::state ||
                                update->type==net::editor_light::kind::ack))
                            {
                                if(update->type==net::editor_light::kind::ack)
                                {
                                    std::lock_guard<std::mutex> lock(data.editor_mutex);
                                    if(!data.editor_outgoing.empty() &&
                                       data.editor_outgoing.front().sequence==
                                           update->sequence)
                                    {
                                        data.editor_outgoing.pop_front();
                                        std::clog<<"clt: editor light "<<update->index
                                                 <<" confirmed by server\n";
                                    }
                                }
                                {
                                    std::lock_guard<std::mutex> lock(data.scene_mutex);
                                    auto& revision=data.light_versions[update->index];
                                    if(net::message::newer(update->revision,revision))
                                    {
                                        revision=update->revision;
                                        data.light_pending[update->index]=*update;
                                    }
                                }
                                last_received=now;
                            }
                            continue;
                        }
                        auto accept_weapon = [&](const net::weapon::snapshot& update)
                        {
                            std::lock_guard<std::mutex> lock(data.weapon_mutex);
                            const auto previous = data.weapon_versions.find(update.weapon_id);
                            if (previous != data.weapon_versions.end() &&
                                (net::message::newer(previous->second.first, update.sequence) ||
                                 (update.sequence == previous->second.first &&
                                  net::message::newer(previous->second.second, update.tick))))
                            {
                                return false;
                            }
                            data.weapon_versions[update.weapon_id] = {update.sequence, update.tick};
                            if (!net::message::newer(data.weapon_ack, update.sequence))
                            {
                                data.weapon_ack = update.sequence;
                                data.weapon_tick = update.tick;
                            }
                            while (!data.weapon_outgoing.empty() &&
                                   !net::message::newer(
                                       data.weapon_outgoing.front().sequence, data.weapon_ack
                                   ))
                            {
                                data.weapon_outgoing.pop_front();
                            }
                            if (data.weapon_updates.size() >= 64)
                            {
                                data.weapon_updates.pop_front();
                            }
                            data.weapon_updates.push_back(update);
                            last_received = now;
                            return true;
                        };
                        if (const auto shot = net::weapon::decode_shot(buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && shot->state.token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                accept_weapon(shot->state);
                                std::lock_guard<std::mutex> lock(data.weapon_mutex);
                                if (data.weapon_shots.size() >= 64)
                                {
                                    data.weapon_shots.pop_front();
                                }
                                data.weapon_shots.push_back(*shot);
                            }
                            continue;
                        }
                        if (const auto update = net::weapon::decode_snapshot(buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && update->token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                accept_weapon(*update);
                            }
                            continue;
                        }

                        if (const auto event = net::animation::decode_event(
                                buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && event->token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                std::lock_guard<std::mutex> lock(data.animation_mutex);
                                if (net::message::newer(event->sequence,
                                                        data.last_attachment_sequence))
                                {
                                    data.last_attachment_sequence = event->sequence;
                                    if (data.attachment_events.size() >= 128)
                                        data.attachment_events.pop_front();
                                    data.attachment_events.push_back(*event);
                                }
                                last_received = now;
                            }
                            continue;
                        }

                        if (const auto state = net::animation::decode(buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && state->token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                std::lock_guard<std::mutex> lock(data.animation_mutex);
                                const auto old = data.animations.find(state->instance_id);
                                if (old == data.animations.end() ||
                                    net::message::newer(state->server_tick, old->second.server_tick))
                                    data.animations[state->instance_id] = *state;
                                last_received = now;
                            }
                            continue;
                        }

                        if (const auto settings = net::movement::decode_parameters(
                                buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && settings->token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                std::lock_guard<std::mutex> lock(data.player_mutex);
                                data.server_parameters = settings->values;
                                last_received = now;
                            }
                            continue;
                        }

                        if (const auto update = net::movement::decode_snapshot(buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && update->token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                // Valid movement traffic proves the server is still alive even
                                // when heartbeat replies are delayed or lost.
                                last_received = now;

                                if (!net::message::newer(update->tick, last_player_tick))
                                {
                                    continue;
                                }

                                last_player_tick = update->tick;
                                data.player_snapshots.fetch_add(1, std::memory_order_relaxed);
                                while (!unacknowledged.empty() &&
                                       !net::message::newer(unacknowledged.front().sequence,
                                                            update->sequence))
                                {
                                    unacknowledged.pop_front();
                                }
                                std::lock_guard<std::mutex> lock(data.player_mutex);
                                data.authority = *update;
                            }

                            continue;
                        }

                        const auto message = net::message::decode(buffer.data(), *size);

                        if(net::endpoint::equal(peer,server) &&
                           transport.accept(peer,buffer.data(),*size,now))
                        {
                            continue;
                        }

                        if (!message || !net::endpoint::equal(peer, server) || message->token != token ||
                            !net::message::newer(message->sequence, last_accepted) ||
                            net::message::newer(message->sequence, outstanding) || !waiting)
                        {
                            continue;
                        }

                        const bool is_connected = data.connected.load(std::memory_order_relaxed);

                        if ((!is_connected && message->type != net::message::type::welcome) ||
                            (is_connected && message->type != net::message::type::state))
                        {
                            continue;
                        }

                        last_accepted = message->sequence;
                        data.server_tick.store(message->value, std::memory_order_relaxed);
                        data.connected.store(true, std::memory_order_relaxed);

                        last_received = now;
                        next_send = now + std::chrono::milliseconds(net::limits::heartbeat_ms);
                        waiting = false;
                        retry = std::chrono::milliseconds(net::limits::retry_ms);
                    }

                    transport.pump(
                        [&](const net::endpoint::resource& peer,const void* bytes,std::size_t size) {
                            return net::secure::send(data.socket,peer,bytes,size);
                        },64*1024,now);
                    net::delivery completed;
                    while(transport.pop(completed))
                    {
                        net::scene_stream::batch_payload batch;
                        if(scene_ready ||
                           !net::scene_stream::decode_batch(completed.payload,batch) ||
                           batch.token!=token||batch.batch!=scene_batch)
                            continue;
                        if(stream_started==clock::time_point{})
                            stream_started=clock::now();
                        ::scene::state received;
                        if(!net::scene_stream::deserialize(batch.scene,received))
                        {
                            clear_scene();
                            next_scene_send=now;
                            continue;
                        }
                        transferred_bytes+=batch.scene.size();
                        {
                            std::lock_guard<std::mutex> lock(data.scene_mutex);
                            std::cout<<"clt: received scene batch "<<scene_batch
                                     <<" ("<<received.meshes.size()<<" meshes, "
                                     <<received.instances.size()<<" instances, "
                                     <<received.textures.size()<<" textures, "
                                     <<received.skins.size()<<" skins)\n";
                            data.pending_scene.push_back(std::move(received));
                        }
                        scene_ready=batch.final;
                        data.scene_complete.store(scene_ready,std::memory_order_relaxed);
                        ++scene_batch;
                        next_scene_send=now;
                        if(scene_ready && stream_started!=clock::time_point{})
                        {
                            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
                                clock::now()-stream_started).count();
                            std::cout<<"clt: scene transferred in "<<elapsed
                                     <<" ms ("<<transferred_bytes/1024<<" KiB)\n";
                        }
                    }


                    if (data.connected.load(std::memory_order_relaxed) && now - last_received > std::chrono::milliseconds(net::limits::connection_timeout_ms))
                    {
                        data.connected.store(false, std::memory_order_relaxed);
                        clear_scene();
                        // Discard the previous DTLS epoch before beginning a fresh
                        // authenticated handshake. Keep the on-disk server pin.
                        net::secure::connect(data.socket,server);

                        // A reconnect is a new player session. Reset the network token so
                        // the server cannot keep the old input acknowledgement sequence.
                        const auto old_token = token;
                        do
                        {
                            token = std::random_device{}() | 1u;
                        } while (token == old_token);

                        sequence = 0;
                        outstanding = 0;
                        last_accepted = 0;
                        last_player_tick = 0;
                        unacknowledged.clear();
                        next_move_send = now;
                        next_scene_send = now;
                        last_received = now;
                        {
                            std::lock_guard<std::mutex> lock(data.player_mutex);
                            data.outgoing.clear();
                            data.authority.reset();
                        }        

                        waiting = false;
                        next_send = now;
                        retry = std::chrono::milliseconds(net::limits::retry_ms);
                    }

                    if (now >= next_send)
                    {
                        send(data.connected.load(std::memory_order_relaxed)
                                ? net::message::type::ping
                                : net::message::type::hello
                        );

                        // A reply enables the next 20 Hz heartbeat. Loss backs retries off to 1
                        // second.
                        if (waiting)
                        {
                            retry = std::min(retry * 2, std::chrono::milliseconds(net::limits::retry_max_ms));
                        }

                        waiting = true;
                        next_send = now + retry;
                    }

                    if (data.connected.load(std::memory_order_relaxed) && !scene_ready && now >= next_scene_send)
                    {
                        const auto packet = net::message::encode({
                            net::message::type::scene_request,
                            ++sequence,
                            token,
                            (scene_batch << 16)
                        });

                        net::secure::send(
                            data.socket,
                            server,
                            packet.data(),
                            packet.size()
                        );

                        if(stream_started==clock::time_point{})
                            stream_started=now;
                        next_scene_send = now + std::chrono::milliseconds(net::limits::scene_request_ms);
                    }

                    if (data.connected.load(std::memory_order_relaxed) && now >= next_weapon_send)
                    {
                        std::lock_guard<std::mutex> lock(data.weapon_mutex);
                        for (auto command : data.weapon_outgoing)
                        {
                            command.token = token;
                            const auto packet = net::weapon::encode_command(command);
                            net::secure::send(data.socket, server, packet.data(), packet.size());
                        }
                        next_weapon_send = now + 30ms;
                    }

                    std::deque<::player::input> inputs;

                    {
                        std::lock_guard<std::mutex> lock(data.player_mutex);

                        for (int i = 0; i < 8 && !data.outgoing.empty(); ++i)
                        {
                            inputs.push_back(data.outgoing.front());
                            data.outgoing.pop_front();
                        }
                    }

                    if (data.connected.load(std::memory_order_relaxed))
                    {
                        for (const auto& input : inputs)
                        {
                            unacknowledged.push_back(input);
                        }

                        if (!unacknowledged.empty() && now >= next_move_send)
                        {
                            auto transmit = [&](const ::player::input& input)
                            {
                                const auto packet = net::movement::encode(token, input);
                                net::secure::send(data.socket, server, packet.data(), packet.size());
                            };

                            // Recover missing early ticks while transmitting current input.
                            for (std::size_t i = 0; i < std::min<std::size_t>(8, unacknowledged.size()); ++i)
                            {
                                transmit(unacknowledged[i]);
                            }

                            if (unacknowledged.size() > 8)
                            {
                                transmit(unacknowledged.back());
                            }

                            next_move_send = now + std::chrono::milliseconds(net::limits::movement_ms);
                        }
                    }
                    else
                    {
                        unacknowledged.clear();
                    }

                    if(data.connected.load(std::memory_order_relaxed) &&
                       now>=next_editor_send)
                    {
                        std::optional<net::editor_light::resource> pending;
                        {
                            std::lock_guard<std::mutex> lock(data.editor_mutex);
                            if(!data.editor_outgoing.empty())
                                pending=data.editor_outgoing.front();
                        }
                        if(pending)
                        {
                            if(pending->sequence!=editor_retry_sequence)
                            {
                                editor_retry_sequence=pending->sequence;
                                editor_retry_attempts=0;
                            }
                            if(editor_retry_attempts++<100u)
                            {
                                pending->token=token;
                                const auto bytes=net::editor_light::encode(*pending);
                                net::secure::send(data.socket,server,
                                                  bytes.data(),bytes.size());
                            }
                            else
                            {
                                // Invalid permissions or rejected edits
                                // cannot hold the command queue indefinitely.
                                std::lock_guard<std::mutex> lock(data.editor_mutex);
                                if(!data.editor_outgoing.empty() &&
                                   data.editor_outgoing.front().sequence==
                                       pending->sequence)
                                    data.editor_outgoing.pop_front();
                                std::clog<<"clt: editor light request not acknowledged "
                                         <<"(check server editor permissions)\n";
                                editor_retry_sequence=0;
                                editor_retry_attempts=0;
                            }
                        }
                        else
                        {
                            editor_retry_sequence=0;
                            editor_retry_attempts=0;
                        }
                        // Retransmit until the matching server ACK arrives.
                        next_editor_send=now+50ms;
                    }

                    auto wake = data.connected.load(std::memory_order_relaxed) && !scene_ready
                            ? std::min(next_send, next_scene_send)
                            : next_send;
                    if (!unacknowledged.empty())
                    {
                        wake = std::min(wake, next_move_send);
                    }

                    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                        wake - clock::now()
                    );

                    net::secure::wait_readable(
                        data.socket,
                        std::clamp(
                            delay,
                            1ms,
                            data.connected.load(std::memory_order_relaxed) ? 5ms : 50ms
                        )
                    );
                }

                if (data.connected.load(std::memory_order_relaxed))
                {
                    send(net::message::type::goodbye);
                }
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(data.error_mutex);
                data.worker_error = std::current_exception();
            }

            data.connected.store(false, std::memory_order_relaxed);
        }
    } // namespace
} // namespace clt::connection



// [clt/platform/vulkan_loader.cpp]

#ifdef __APPLE__
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace
{
    // GLFW 3.4 cannot find Homebrew's or SDK's Vulkan dylib by its standard name.
    // Keep the selected library loaded for the duration of the process.
    PFN_vkGetInstanceProcAddr discover_loader()
    {
        std::vector<std::string> paths;

        if (const char* sdk = std::getenv("VULKAN_SDK"))
        {
            paths.emplace_back(std::string(sdk) + "/lib/libMoltenVK.dylib");
            paths.emplace_back(std::string(sdk) + "/lib/libvulkan.1.dylib");
        }

        if (const char* prefix = std::getenv("HOMEBREW_PREFIX"))
        {
            paths.emplace_back(std::string(prefix) + "/opt/molten-vk/lib/libMoltenVK.dylib");
            paths.emplace_back(std::string(prefix) + "/lib/libMoltenVK.dylib");
            paths.emplace_back(std::string(prefix) + "/lib/libvulkan.1.dylib");
        }

        for (const char* path :
             {"/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
              "/usr/local/opt/molten-vk/lib/libMoltenVK.dylib",
              "/opt/homebrew/lib/libMoltenVK.dylib",
              "/usr/local/lib/libMoltenVK.dylib",
              "libMoltenVK.dylib",
              "/opt/homebrew/lib/libvulkan.1.dylib",
              "/usr/local/lib/libvulkan.1.dylib",
              "libvulkan.1.dylib"})
        {
            paths.emplace_back(path);
        }

        for (const auto& path : paths)
        {
            void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

            if (!library)
            {
                continue;
            }

            auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(library, "vkGetInstanceProcAddr")
            );

            if (proc)
            {
                clt::vulkan_loader::get()->library = library;

                return proc;
            }

            dlclose(library);
        }

        return nullptr;
    }
} // namespace
#endif

namespace clt::vulkan_loader
{
    state* get()
    {
        static state data;
        return &data;
    }

    PFN_vkGetInstanceProcAddr init()
    {
        if (!get()->initialized)
        {
#ifdef __APPLE__
            get()->get_instance_proc_addr = discover_loader();
#endif
            get()->initialized = true;
        }

        return get()->get_instance_proc_addr;
    }

    void shutdown() noexcept
    {
#ifdef __APPLE__
        if (get()->library)
        {
            dlclose(get()->library);
        }
#endif
        *get() = {};
    }
} // namespace clt::vulkan_loader



// [clt/platform/window.cpp]





#include <atomic>
#include <stdexcept>

namespace clt::window
{
    namespace
    {
        std::atomic<int> framebuffer_width{};
        std::atomic<int> framebuffer_height{};

        void update_framebuffer_size()
        {
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(get()->handle, &width, &height);
            framebuffer_width.store(width, std::memory_order_relaxed);
            framebuffer_height.store(height, std::memory_order_relaxed);
        }
    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        if (get()->initialized)
        {
            throw std::runtime_error("clt::window already initialized");
        }

#ifdef __APPLE__
        if (const auto loader = vulkan_loader::init())
        {
            glfwInitVulkanLoader(loader);
        }
#endif

        if (!glfwInit())
        {
            throw std::runtime_error("GLFW initialization failed");
        }

        get()->initialized = true;
        get()->width = game::options::get()->graphics.width;
        get()->height = game::options::get()->graphics.height;
        get()->title = game::options::get()->graphics.title;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        get()->handle =
            glfwCreateWindow(get()->width, get()->height, get()->title.c_str(), nullptr, nullptr);

        if (!get()->handle)
        {
            shutdown();
            throw std::runtime_error("Window creation failed");
        }

        update_framebuffer_size();
    }

    bool should_close()
    {
        return glfwWindowShouldClose(get()->handle);
    }

    void framebuffer_size(int& width, int& height)
    {
        width = framebuffer_width.load(std::memory_order_relaxed);
        height = framebuffer_height.load(std::memory_order_relaxed);
    }

    bool minimized()
    {
        int width, height;
        framebuffer_size(width, height);
        return width == 0 || height == 0;
    }

    void poll_events()
    {
        glfwPollEvents();
        update_framebuffer_size();
    }

    void shutdown() noexcept
    {
        if (!get()->initialized)
        {
            return;
        }

        if (get()->handle)
        {
            glfwDestroyWindow(get()->handle);
        }

        glfwTerminate();
        *get() = {};
    }
} // namespace clt::window



// [clt/player/prediction.cpp]







#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include <cmath>

namespace clt::prediction
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        struct state {
            std::atomic<bool> stop{false};

            std::atomic<std::uint32_t> buttons{0};
            std::atomic<float> yaw{0};
            std::atomic<std::uint64_t> ticks{0};
            std::atomic<float> error{0};

            std::mutex pose_mutex;
            vk::util::math::vec3 pose{};

            std::thread worker;
        };

        state& get()
        {
            static state data;
            return data;
        }

        void run()
        {
            auto& data = get();

            std::shared_ptr<const ::scene::state> cached_scene;
            ::player::collision_world colliders;
            spatial::index collision_index;

            ::player::state avatar;
            vk::util::math::vec3 visual_offset{};
            std::deque<::player::input> pending;

            std::uint32_t sequence = 0;
            std::uint32_t last_server_tick = 0;

            bool ready = false;
            bool configured = false;

            auto next = clock::now();
            const auto tick = std::chrono::nanoseconds(1'000'000'000 / net::limits::tick_hz);

            while (!data.stop.load(std::memory_order_relaxed))
            {
                const auto source = connection::snapshot();
                const bool connected = connection::get()->connected.load(std::memory_order_relaxed);
                const auto parameters = connection::movement_parameters();

                if (!connected || !source || !parameters)
                {
                    cached_scene.reset();
                    colliders.clear();
                    collision_index = {};
                    avatar = {};
                    configured = false;
                    visual_offset = {};
                    pending.clear();
                    sequence = 0;
                    last_server_tick = 0;
                    ready = false;
                }
                else
                {
                    if (!configured)
                    {
                        ::player::configure(*parameters);
                        avatar.position = parameters->spawn;
                        configured = true;
                    }
                }
                if (connected && source && parameters && source != cached_scene)
                {
                    // New batches add collision geometry without resetting
                    // the player's input sequence or rebuilding old BVH nodes.
                    bool appended = cached_scene &&
                        source->meshes.size() == cached_scene->meshes.size() &&
                        source->instances.size() >= cached_scene->instances.size();

                    if (appended)
                    {
                        for (std::size_t i = 0; i < source->meshes.size(); ++i)
                        {
                            if (!::scene::same_mesh(
                                source->meshes[i], cached_scene->meshes[i]))
                            {
                                appended = false;
                                break;
                            }
                        }
                    }
                    if (appended)
                    {
                        for (std::size_t i = 0; i < cached_scene->instances.size(); ++i)
                        {
                            if (!::scene::same_instance(
                                source->instances[i], cached_scene->instances[i]))
                            {
                                appended = false;
                                break;
                            }
                        }
                    }

                    if (appended)
                    {
                        const auto start = cached_scene->instances.size();
                        std::vector<::scene::instance> incoming(
                            source->instances.begin() + start, source->instances.end()
                        );
                        auto added = ::player::build_colliders(source->meshes, incoming);
                        std::vector<spatial::bounds> boxes;
                        boxes.reserve(added.size());
                        for (const auto& box : added)
                        {
                            boxes.push_back({box.min, box.max});
                        }
                        colliders.insert(colliders.end(), added.begin(), added.end());
                        collision_index.append(boxes);
                    }
                    else
                    {
                        colliders = ::player::build_colliders(*source);
                        collision_index = ::player::build_index(colliders);
                    }
                    cached_scene = source;
                }

                if (connected && source && parameters)
                {
                    if (const auto update = connection::take_authority())
                    {
                        if (!ready || net::message::newer(update->tick, last_server_tick))
                        {
                            const auto previous = avatar.position;
                            const bool was_ready = ready;
                            avatar = update->player;
                            last_server_tick = update->tick;
                            ready = true;

                            while (!pending.empty() && !net::message::newer(pending.front().sequence, update->sequence))
                            {
                                pending.pop_front();
                            }

                            for (const auto& command : pending)
                            {
                                ::player::step(avatar, command, colliders, &collision_index);
                            }

                            if (was_ready)
                            {
                                const auto delta = previous - avatar.position;
                                const auto distance = vk::util::math::length(delta);
                                data.error.store(distance, std::memory_order_relaxed);
                                if (distance < 2.0f)
                                {
                                    visual_offset = visual_offset + delta;
                                }
                                else
                                {
                                    visual_offset = {};
                                }
                            }
                        }
                    }

                    if (ready)
                    {
                        ::player::input input;

                        input.sequence = ++sequence;

                        if (input.sequence == 0)
                        {
                            input.sequence = ++sequence;
                        }

                        input.buttons =
                            data.buttons.load(std::memory_order_relaxed);

                        input.yaw =
                            data.yaw.load(std::memory_order_relaxed);

                        ::player::step(avatar, input, colliders, &collision_index);
                        data.ticks.fetch_add(1, std::memory_order_relaxed);

                        pending.push_back(input);

                        if (pending.size() > 128)
                        {
                            pending.pop_front();
                        }

                        connection::send_input(input);
                    }
                }

                visual_offset = visual_offset * 0.8f;
                {
                    std::lock_guard<std::mutex> lock(data.pose_mutex);
                    data.pose = avatar.position + visual_offset;
                }

                next += tick;

                if (next < clock::now())
                {
                    next = clock::now();
                }

                std::this_thread::sleep_until(next);
            }
        }
    } // namespace

    void init()
    {
        auto& data = get();

        data.stop.store(false, std::memory_order_relaxed);
        data.buttons.store(0, std::memory_order_relaxed);
        data.yaw.store(0, std::memory_order_relaxed);
        data.ticks.store(0, std::memory_order_relaxed);
        data.error.store(0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(data.pose_mutex);
            data.pose = {};
        }
        data.worker = std::thread(run);
    }

    void controls(std::uint32_t buttons, float yaw)
    {
        get().buttons.store(buttons, std::memory_order_relaxed);
        get().yaw.store(yaw, std::memory_order_relaxed);
    }

    vk::util::math::vec3 position()
    {
        auto& data = get();

        std::lock_guard<std::mutex> lock(data.pose_mutex);
        return data.pose;
    }

    std::uint64_t ticks()
    {
        return get().ticks.load(std::memory_order_relaxed);
    }

    float correction()
    {
        return get().error.load(std::memory_order_relaxed);
    }

    void shutdown()
    {
        auto& data = get();

        data.stop.store(true, std::memory_order_relaxed);

        if (data.worker.joinable())
        {
            data.worker.join();
        }
    }
} // namespace clt::prediction



// [clt/render/postprocess.cpp]


// [clt/render/scene_renderer.hpp]

#include <vk/vk.hpp>


#include <array>
#include <cstddef>
#include <vector>
#include <cstdint>

namespace clt::scene_renderer
{
    struct gbuffer {
        std::array<vk::ImageView, 4> views{};
        vk::Extent2D extent{};
    };

    gbuffer outputs(std::uint32_t image);

    bool needs_sync();
    void create_frame_resources();
    void release_frame_resources();
    
    void init(const char* vertex_path, const char* fragment_path);
    void synchronize();
    void draw(vk::CommandBuffer command, std::uint32_t image,
              const camera::frame& camera);

    void shutdown();
} // namespace clt::scene_renderer





#include <pcloud/pcloud.hpp>
#include <editor/editor.hpp>
#include <editor/vulkan.hpp>
#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace clt::postprocess
{
    namespace
    {
        std::shared_ptr<const ::scene::state> registered;
        // Keep the early-out conservative: pc:: computes indirect light
        // up to 2.6 world units beyond a point light's direct radius.
        constexpr float bounce_reach=2.6f;
        pc::camera_frame convert_camera(const camera::frame& frame)
        {
            return {frame.eye,frame.pose.yaw,frame.pose.pitch,frame.pose.fov};
        }

        // Skip expensive world conversion when appended meshes do not alter
        // the light volume or its possible shadow casters. Copied unchanged
        // from the previous engine's CPU light-relevance optimization.
        bool lighting_unchanged(const ::scene::state& previous,
                                const ::scene::state& incoming)
        {
            if(incoming.instances.size()<previous.instances.size() ||
               incoming.meshes.size()!=previous.meshes.size() ||
               incoming.lights.size()!=previous.lights.size() ||
               incoming.collision_boxes.size()!=previous.collision_boxes.size() ||
               incoming.skins.size()!=previous.skins.size())
                return false;
            for(std::size_t i=0;i<previous.lights.size();++i)
            {
                const auto& a=previous.lights[i],&b=incoming.lights[i];
                if(a.position.x!=b.position.x || a.position.y!=b.position.y ||
                   a.position.z!=b.position.z || a.radius!=b.radius ||
                   a.color.x!=b.color.x || a.color.y!=b.color.y ||
                   a.color.z!=b.color.z || a.strength!=b.strength)
                    return false;
            }
            for(std::size_t i=0;i<previous.collision_boxes.size();++i)
            {
                const auto& a=previous.collision_boxes[i],&b=incoming.collision_boxes[i];
                if(a.minimum.x!=b.minimum.x || a.minimum.y!=b.minimum.y ||
                   a.minimum.z!=b.minimum.z || a.maximum.x!=b.maximum.x ||
                   a.maximum.y!=b.maximum.y || a.maximum.z!=b.maximum.z)
                    return false;
            }
            for(std::size_t i=0;i<previous.meshes.size();++i)
                if(!::scene::same_mesh(previous.meshes[i],incoming.meshes[i]))
                    return false;
            for(std::size_t i=0;i<previous.skins.size();++i)
            {
                const auto& a=previous.skins[i],&b=incoming.skins[i];
                if(a.id!=b.id || a.skeleton!=b.skeleton ||
                   a.inverse_bind!=b.inverse_bind || a.clips.size()!=b.clips.size())
                    return false;
                for(std::size_t j=0;j<a.clips.size();++j)
                    if(a.clips[j].name!=b.clips[j].name ||
                       a.clips[j].bytes!=b.clips[j].bytes)
                        return false;
            }
            if(!std::equal(previous.instances.begin(),previous.instances.end(),
                           incoming.instances.begin(),::scene::same_instance))
                return false;
        
            using namespace vk::util::math;
            for(std::size_t i=previous.instances.size();i<incoming.instances.size();++i)
            {
                const auto& instance=incoming.instances[i];
                const auto found=std::find_if(incoming.meshes.begin(),incoming.meshes.end(),
                    [&](const ::scene::mesh& mesh){return mesh.id==instance.mesh_id;});
                if(found==incoming.meshes.end())return false;
                if(found->surface.alpha_mode==2 || found->indices.empty())continue;
                // Animated deformation can extend outside the bind-pose bounds.
                // Conservatively force a rebuild for new skinned instances.
                if(found->skin_id)return false;
                float furthest_squared=0.f;
                for(const auto& vertex:found->vertices)
                    furthest_squared=std::max(furthest_squared,
                        length_squared(vertex.position));
                const float scale=std::max({std::abs(instance.pose.scale.x),
                    std::abs(instance.pose.scale.y),std::abs(instance.pose.scale.z)});
                const auto& rotation=instance.pose.rotation;
                const float rotation_length_squared=
                    rotation.x*rotation.x+rotation.y*rotation.y+
                    rotation.z*rotation.z+rotation.w*rotation.w;
                if(!std::isfinite(scale) || !std::isfinite(furthest_squared) ||
                   !std::isfinite(rotation_length_squared) ||
                   std::abs(rotation_length_squared-1.f)>0.00001f)
                    return false;
                const float radius=std::sqrt(furthest_squared)*scale+0.01f;
                for(const auto& light:incoming.lights)
                {
                    // Indirect rays can travel bounce_reach past the lit surface.
                    const float reach=radius+light.radius+bounce_reach;
                    if(length_squared(instance.pose.position-light.position)<=reach*reach)
                        return false;
                }
            }
            return true;
        }

        std::shared_ptr<pc::world> prepare_world(const ::scene::state* source)
        {
            auto prepared=std::make_shared<pc::world>();
            if(!source)return prepared;
            using namespace vk::util::math;
            prepared->lights.reserve(source->lights.size());
            for(const auto& light:source->lights)
                prepared->lights.push_back({
                    light.position,light.radius,light.color,
                    light.enabled?light.strength:0.f
                });
            prepared->collision_boxes.reserve(source->collision_boxes.size());
            for(const auto& box:source->collision_boxes)
                prepared->collision_boxes.push_back({box.minimum,box.maximum});

            // Game-only animation and mesh flattening. pc:: never depends on
            // scene::mesh, Ozz or any particular renderer/scene ownership.
            // Shadow casters use rendered mesh triangles, not collision proxies.
            // Transform each streamed instance into the same world coordinates
            // used by mesh.vert; alpha-blended surfaces do not occlude light.
            std::unordered_map<std::uint32_t,const ::scene::mesh*> mesh_lookup;
            mesh_lookup.reserve(source->meshes.size());
            for(const auto& mesh:source->meshes)mesh_lookup.emplace(mesh.id,&mesh);
            for(const auto& instance:source->instances)
            {
                const auto found=mesh_lookup.find(instance.mesh_id);
                if(found==mesh_lookup.end())continue;
                const auto* mesh=found->second;
                if(mesh->surface.alpha_mode==2)continue;
                std::vector<animation::matrix> palette;
                if(mesh->skin_id && !animation::sample(
                       mesh->skin_id,instance.animation,0.0f,palette,
                       instance.overlay_animation,instance.overlay_weight,
                       instance.overlay_root))
                    throw std::runtime_error("Cannot sample imported shadow caster skin");

                const auto transformed=[&](const ::scene::vertex& v) {
                    vec3 local=v.position;
                    if(!palette.empty())
                    {
                        local={};
                        for(std::size_t j=0;j<4;++j)
                        {
                            if(v.weights[j]<=0.0f)continue;
                            if(v.joints[j]>=palette.size())
                                throw std::runtime_error("Shadow caster joint index invalid");
                            const auto& m=palette[v.joints[j]];
                            const vec3 p=v.position;
                            local=local+vec3{
                                m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
                                m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
                                m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]
                            }*v.weights[j];
                        }
                    }
                    const auto& scale=instance.pose.scale;
                    return instance.pose.position+rotate(instance.pose.rotation,{
                        local.x*scale.x,local.y*scale.y,local.z*scale.z
                    });
                };
                std::vector<vec3> positions;
                positions.reserve(mesh->vertices.size());
                for(const auto& vertex:mesh->vertices)
                    positions.push_back(transformed(vertex));
                for(std::size_t i=0;i+2<mesh->indices.size();i+=3)
                {
                    const vec3 a=positions.at(mesh->indices[i]);
                    const vec3 b=positions.at(mesh->indices[i+1]);
                    const vec3 c=positions.at(mesh->indices[i+2]);
                    if(length_squared(cross(b-a,c-a))<1e-12f)continue;
                    // Preserve the original scene-streaming CPU budget.
                    // The relevance rule lives in pc::, never in the game.
                    if(!pc::relevant_triangle({a,b,c},prepared->lights))
                        continue;
                    prepared->triangles.push_back({a,b,c});
                }
            }
            return prepared;
        }
    }

    void set_exposure(float exposure){pc::set_exposure(exposure);}
    void toggle_probes(){pc::toggle_probes();}
    void updated(std::uint32_t image){pc::updated(image);}

    void create()
    {
        std::vector<pc::gbuffer> gbuffers;
        const auto count=static_cast<std::uint32_t>(
            vk::swapchain::get()->images.size());
        gbuffers.reserve(count);
        for(std::uint32_t i=0;i<count;++i)
            gbuffers.push_back(scene_renderer::outputs(i).views);
        pc::configuration shaders{};
        const auto& configured=game::options::get()->graphics;
        shaders.lighting_shader=configured.lighting_shader;
        shaders.tonemap_shader=configured.tonemap_shader;
        shaders.fullscreen_shader=configured.fullscreen_shader;
        shaders.presentation_shader=configured.presentation_shader;
        pc::presentation_target surface{};
        surface.images=vk::swapchain::get()->images;
        surface.format=vk::swapchain::get()->format;
        surface.extent=vk::swapchain::get()->extent;
        pc::create(shaders,gbuffers,surface);
    }

    void synchronize_scene()
    {
        const auto source=connection::snapshot();
        if(source==registered)return;
        if(source && registered && lighting_unchanged(*registered,*source))
        {
            registered=source;
            return;
        }
        pc::set_scene(prepare_world(source.get()));
        pc::synchronize_scene();
        registered=source;
    }

    void release()
    {
        pc::release();
        registered.reset();
    }

    targets images(std::uint32_t image)
    {
        const auto output=pc::images(image);
        return {output.grid,output.points,output.hdr,output.ldr};
    }
    void cloud(vk::CommandBuffer cmd,std::uint32_t image)
    {
        pc::cloud(cmd,image);
    }
    void gi(vk::CommandBuffer cmd,std::uint32_t image)
    {
        pc::gi(cmd,image);
    }
    void lighting(vk::CommandBuffer cmd,std::uint32_t image,
                  const camera::frame& frame)
    {
        pc::lighting(cmd,image,convert_camera(frame));
    }
    void tonemap(vk::CommandBuffer cmd,std::uint32_t image,
                 const camera::frame& frame)
    {
        pc::tonemap(cmd,image,convert_camera(frame));
    }
    void present(vk::CommandBuffer cmd,std::uint32_t image)
    {
        pc::present(cmd,image,[](vk::CommandBuffer command,vk::Extent2D extent)
        {
            // Original game-specific editor stays in the host's presentation.
            editor::vulkan::record(command,
                editor::instance().paint(int(extent.width),int(extent.height)),
                extent);
        });
    }
}



// [clt/render/renderer.cpp]





#include <gpu/graph.hpp>
#include <gpu/upload.hpp>
#include <editor/editor.hpp>
#include <editor/vulkan.hpp>





#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <thread>

namespace clt::renderer
{
    namespace
    {
        static vk::Extent2D framebuffer_extent(vk::SurfaceCapabilitiesKHR caps)
        {
            if (caps.currentExtent.width != UINT32_MAX)
            {
                return caps.currentExtent;
            }

            int width, height;
            window::framebuffer_size(width, height);

            return {
                std::clamp(
                    static_cast<uint32_t>(width),
                    caps.minImageExtent.width,
                    caps.maxImageExtent.width
                ),
                std::clamp(
                    static_cast<uint32_t>(height),
                    caps.minImageExtent.height,
                    caps.maxImageExtent.height
                )
            };
        }

    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void init(const char* vertex_path, const char* fragment_path)
    {
        if (get()->initialized)
        {
            throw std::runtime_error("clt::renderer already initialized");
        }

        if (!window::get()->handle)
        {
            throw std::runtime_error("clt::window must be initialized before the renderer");
        }

        get()->initialized = true;

        try
        {
            // Decode the original 90-glyph editor font synchronously once,
            // BEFORE the render thread exists. No first-frame font stall.
            editor::vulkan::initialize_font();
            editor::instance().set_font_metrics(
                editor::vulkan::font_metrics(),16);
            if (!glfwVulkanSupported())
            {
#ifdef __APPLE__
                throw std::runtime_error(
                    "Vulkan runtime not available. Install MoltenVK with: brew install molten-vk "
                    "(or install and configure the macOS Vulkan SDK)"
                );
#else
                throw std::runtime_error("Vulkan loader or driver not available");
#endif
            }
#ifdef __APPLE__
            if (!vk::loader::init(vulkan_loader::init()))
            {
                throw std::runtime_error("Vulkan loader initialization failed");
            }
#else
            if (!vk::loader::init())
            {
                throw std::runtime_error("Vulkan loader initialization failed");
            }
#endif

            uint32_t count = 0;
            const char** required = glfwGetRequiredInstanceExtensions(&count);

            if (!required || !count)
            {
                throw std::runtime_error("No Vulkan surface extensions");
            }

            std::vector<const char*> extensions(required, required + count);
            bool portability = false;

            for (const auto& extension : vk::instance::extensions())
            {
                if (std::strcmp(
                        extension.extensionName.data(),
                        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
                    ) == 0)
                {
                    portability = true;
                    break;
                }
            }

            if (portability)
            {
                extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            }

            vk::ApplicationInfo application{};
            application.pApplicationName = "vulkan-game";
            application.apiVersion = VK_API_VERSION_1_0;

            vk::InstanceCreateInfo instance_info{};
            instance_info.pApplicationInfo = &application;
            instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            instance_info.ppEnabledExtensionNames = extensions.data();

            if (portability)
            {
                instance_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
            }

            vk::instance::init(instance_info);

            VkSurfaceKHR raw_surface = VK_NULL_HANDLE;

            if (glfwCreateWindowSurface(
                    static_cast<VkInstance>(vk::instance::get()->handle),
                    window::get()->handle,
                    nullptr,
                    &raw_surface
                ) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create Vulkan surface");
            }

            vk::surface::init(vk::SurfaceKHR(raw_surface), true);

            uint32_t family = UINT32_MAX;

            for (auto gpu : vk::instance::physical_devices())
            {
                bool swapchain = false;

                for (const auto& extension : gpu.enumerateDeviceExtensionProperties())
                {
                    if (std::strcmp(
                            extension.extensionName.data(), VK_KHR_SWAPCHAIN_EXTENSION_NAME
                        ) == 0)
                    {
                        swapchain = true;
                    }
                }

                if (!swapchain)
                {
                    continue;
                }

                const auto queues = gpu.getQueueFamilyProperties();

                for (uint32_t i = 0; i < queues.size(); ++i)
                {
                    if (queues[i].queueCount &&
                        (queues[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                        (queues[i].queueFlags & vk::QueueFlagBits::eCompute) &&
                        gpu.getSurfaceSupportKHR(i, vk::surface::get()->handle))
                    {
                        vk::physical_device::adopt(gpu);
                        family = i;
                        break;
                    }
                }

                if (family != UINT32_MAX)
                {
                    break;
                }
            }

            if (family == UINT32_MAX)
            {
                throw std::runtime_error("GPU culling requires a graphics, compute and present Vulkan queue");
            }

            float priority = 1.0f;
            vk::DeviceQueueCreateInfo queue_info{};
            queue_info.queueFamilyIndex = family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;

            std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            constexpr const char* portability_subset = "VK_KHR_portability_subset";

            if (vk::physical_device::supports_extension(portability_subset))
            {
                device_extensions.push_back(portability_subset);
            }

            vk::DeviceCreateInfo device_info{};
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;
            device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
            device_info.ppEnabledExtensionNames = device_extensions.data();
            vk::device::init(device_info);
            vk::queue::init(family);
            vk::command::init(family);
            get()->timestamps_supported=
                vk::physical_device::get()->handle
                    .getQueueFamilyProperties()[family].timestampValidBits>0;

            get()->frames.create(2);
                
            scene_renderer::init(vertex_path, fragment_path);

            resize();
        }
        catch (...)
        {
            shutdown();
            throw;
        }
    }

    void resize()
    {
        auto& data = *get();


        vk::device::idle();
        data.timings.reset();
        gpu::upload::retire(true);

        data.passes.reset();
        postprocess::release();
        scene_renderer::release_frame_resources();
        const auto caps = vk::surface::capabilities();

        if (!(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eColorAttachment))
        {
            throw std::runtime_error("Surface does not support color attachments");
        }

        const auto choice = game::options::get()->graphics.surface;
        const auto requested_format =
            choice == game::options::surface_format::bgra8_srgb
                ? vk::Format::eB8G8R8A8Srgb : vk::Format::eR8G8B8A8Srgb;
        const auto formats = vk::surface::formats();
        auto selected = std::find_if(formats.begin(), formats.end(),
            [&](const vk::SurfaceFormatKHR& value)
            {
                return value.format == requested_format &&
                    value.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
            });
        // Both common SRGB image formats use the same presentation path.
        if(selected == formats.end())
            selected = std::find_if(formats.begin(), formats.end(),
                [](const vk::SurfaceFormatKHR& value)
                {
                    return (value.format == vk::Format::eB8G8R8A8Srgb ||
                            value.format == vk::Format::eR8G8B8A8Srgb) &&
                            value.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
                });
        if(selected == formats.end())
            throw std::runtime_error("No supported SRGB swapchain surface format");
        const auto format = *selected;

        vk::SwapchainCreateInfoKHR info{};
        info.surface = vk::surface::get()->handle;
        info.minImageCount = caps.minImageCount + 1;

        if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
        {
            info.minImageCount = caps.maxImageCount;
        }

        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = framebuffer_extent(caps);
        info.imageArrayLayers = 1;
        info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        info.imageSharingMode = vk::SharingMode::eExclusive;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        const auto preferred = game::options::get()->graphics.present;
        info.presentMode = preferred == game::options::presentation::fifo
            ? vk::PresentModeKHR::eFifo
            : preferred == game::options::presentation::mailbox
                ? vk::PresentModeKHR::eMailbox
                : vk::PresentModeKHR::eImmediate;
        const auto modes = vk::surface::present_modes();
        if (std::find(modes.begin(), modes.end(), info.presentMode) == modes.end())
            throw std::runtime_error("Configured presentation mode unsupported");

        info.clipped = true;
        vk::swapchain::init(info);
        scene_renderer::create_frame_resources();
        postprocess::create();
        postprocess::synchronize_scene();
        data.passes.resize(vk::swapchain::get()->images.size());

        data.frames.resize(vk::swapchain::get()->size);
        data.timings.create(
            static_cast<std::uint32_t>(vk::swapchain::get()->images.size()),
            6,data.timestamps_supported);
    }

    void draw_frame()
    {
        using clock=std::chrono::steady_clock;
        static auto previous=clock::now();
        const auto now=clock::now();
        const double elapsed=std::chrono::duration<double,std::milli>(
            now-previous).count();
        previous=now;
        if(editor::instance().visible())
            editor::instance().set_frame_time(elapsed);
        auto& data = *get();


        int width, height;
        window::framebuffer_size(width, height);

        if (!width || !height)
        {
            return;
        }

        if (framebuffer_extent(vk::surface::capabilities()) !=
            vk::swapchain::get()->extent)
        {
            resize();

            return;
        }

        data.frames.wait();

        // Poll without waiting on GPU queries. Each image owns its own pool.
        for(std::uint32_t image=0;image<vk::swapchain::get()->images.size();++image)
        {
            auto values=data.timings.read(image);
            if(values.size()==data.gpu_ms.size())
                std::copy(values.begin(),values.end(),data.gpu_ms.begin());
        }
        if(++data.profile_frames%120==0 && data.timings.supported())
        {
            std::clog << "gpu(ms): geometry=" << data.gpu_ms[0]
                      << " cloud=" << data.gpu_ms[1]
                      << " gi=" << data.gpu_ms[2]
                      << " lighting=" << data.gpu_ms[3]
                      << " tonemap=" << data.gpu_ms[4]
                      << " present=" << data.gpu_ms[5] << '\n';
        }

        // Resource replacements are only applied once all older submissions
        // have finished. Never block the render thread waiting on every frame.
        gpu::upload::retire();
        if (scene_renderer::needs_sync())
        {
            const bool safe = gpu::upload::ready() && data.frames.idle();
            if (!safe)
            {
                std::this_thread::yield();
                return;
            }
            scene_renderer::synchronize();
            postprocess::synchronize_scene();
            data.passes.invalidate();
        }

        ::render::frame_scheduler::ticket active{};
        try
        {
            active = data.frames.begin();
        }
        catch (const vk::OutOfDateKHRError&)
        {
            resize();
            return;
        }

        // Geometry retains its render-pass external dependency into compute.
        // All subsequent image hazards are tracked by the reusable GPU graph.
        const auto render_camera=camera::capture();
        const auto target=postprocess::images(active.image);
        const vk::ImageSubresourceRange range{
            vk::ImageAspectFlagBits::eColor,0,1,0,1
        };
        data.timings.begin_frame(active.command,active.image);
        const auto measure=[&](std::uint32_t index,gpu::graph::callback callback)
            ->gpu::graph::callback
        {
            return [timings=&data.timings,image=active.image,index,
                    callback=std::move(callback)](vk::CommandBuffer command)
            {
                timings->start(command,image,index);
                callback(command);
                timings->end(command,image,index);
            };
        };
        data.passes.record(active.command,active.image,{
            {"geometry",{},measure(0,[image=active.image,render_camera](vk::CommandBuffer cmd)
                {scene_renderer::draw(cmd,image,render_camera);})},
            {"point-cloud",{gpu::access::for_buffer(target.points,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite,true)},
                measure(1,[image=active.image](vk::CommandBuffer cmd)
                {postprocess::cloud(cmd,image);})},
            {"probe-grid",{
                gpu::access::for_buffer(target.points,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead,false),
                gpu::access::for_image(target.grid,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,true)},
                measure(2,[image=active.image](vk::CommandBuffer cmd)
                {postprocess::gi(cmd,image);})},
            {"lighting",{
                gpu::access::for_image(target.grid,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead,false),
                gpu::access::for_image(target.hdr,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,true)},
                measure(3,[image=active.image,render_camera](vk::CommandBuffer cmd)
                {postprocess::lighting(cmd,image,render_camera);})},
            {"tonemapping",{
                gpu::access::for_image(target.hdr,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead,false),
                gpu::access::for_image(target.ldr,range,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,true)},
                measure(4,[image=active.image,render_camera](vk::CommandBuffer cmd)
                {postprocess::tonemap(cmd,image,render_camera);})},
            {"presentation",{gpu::access::for_image(target.ldr,range,
                vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits::eFragmentShader,
                vk::AccessFlagBits::eShaderRead,false)},
                measure(5,[image=active.image](vk::CommandBuffer cmd)
                {postprocess::present(cmd,image);})}
        });

        try
        {
            auto result = data.frames.submit(active);
            data.timings.submitted(active.image);
            if(result==vk::Result::eSuccess || result==vk::Result::eSuboptimalKHR)
                postprocess::updated(active.image);

            if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR)
            {
                resize();
            }
        }
        catch (const vk::OutOfDateKHRError&)
        {
            resize();
        }
    }

    void shutdown() noexcept
    {
        if (!get()->initialized)
        {
            return;
        }

        const auto device = vk::device::get()->handle;

        if (device)
        {
            try
            {
                vk::device::idle();
                gpu::upload::retire(true);
            }
            catch (...)
            {
                // Device loss must not prevent resource cleanup.
            }

            get()->frames.reset();
            get()->passes.reset();
            get()->timings.reset();
            postprocess::release();
            scene_renderer::shutdown();

            vk::command::shutdown();
            vk::swapchain::shutdown();
            vk::queue::shutdown();
            vk::device::shutdown();
        }

        vk::physical_device::shutdown();
        vk::surface::shutdown();
        vk::instance::shutdown();
        vk::loader::shutdown();
        get()->initialized = false;
    }
} // namespace clt::renderer



// [clt/render/scene_renderer.cpp]





// [clt/render/texture.hpp]



#include <vk/vk.hpp>

namespace clt::texture
{
    void init();
    void synchronize(const ::scene::state& source);
    void bind(vk::DescriptorSet set, const ::scene::material& surface);
    void pin(const ::scene::state& source);
    void reset();
    void shutdown();
} // namespace clt::texture


#include <gpu/upload.hpp>
#include <gpu/culler.hpp>
#include <gpu/resource.hpp>
#include <gpu/descriptors.hpp>
#include <render/renderer.hpp>
#include <render/geometry.hpp>
#include <render/pipeline.hpp>
#include <render/target.hpp>
#include <render/attachments.hpp>






#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <bit>

namespace clt::scene_renderer
{
    namespace
    {
        using namespace vk::util::math;

        struct instance_data {
            mat4 model;
            mat4 normal;
        };

        struct draw_parameters {
            mat4 view_projection;
            std::array<float, 4> base_color;
            std::array<float, 4> surface;
            std::array<float, 4> emissive;
            std::uint32_t joint_count = 0;
        };
        static_assert(sizeof(draw_parameters) <= 128);

        struct gpu_mesh {
            std::uint32_t id = 0;
            ::scene::material surface;
            std::uint32_t skin_id = 0;
            std::uint32_t joint_count = 0;
            ::render::geometry geometry;
            gpu::owned_buffer instance_buffer;
            gpu::owned_buffer bounds_buffer;

            struct per_image {
                gpu::owned_buffer visible;
                gpu::owned_buffer indirect;
                gpu::owned_buffer skin_palette;
                gpu::owned_buffer local_instances;
                vk::DescriptorSet set{};
            };
            std::vector<per_image> frames;
            std::vector<std::array<float, 4>> bounds;
            std::vector<instance_data> transforms;
            std::vector<::scene::instance> instances;
            vec3 center;
            float radius = 0;
            std::uint32_t index_count = 0;
        };

        static_assert(sizeof(instance_data) == 128);
        static_assert(sizeof(::scene::vertex) == 64);

        struct cull_parameters {
            std::array<vec4, 6> planes{};
            std::uint32_t count = 0;
            std::uint32_t index_count = 0;
        };
        static_assert(sizeof(cull_parameters) <= 128);

        struct state {
            std::shared_ptr<const ::scene::state> snapshot;
            std::vector<gpu_mesh> meshes;
            std::vector<gpu_mesh> local_meshes;
            std::unique_ptr<gpu::descriptors> local_sets;
            vk::DescriptorSetLayout instance_layout{};
            std::unique_ptr<gpu::descriptors> instance_sets;
            gpu::culler cull;
            std::uint32_t last_animation_tick = UINT32_MAX;
            std::chrono::steady_clock::time_point animation_received{};

            vk::RenderPass render_pass{};
            vk::PipelineLayout layout{};
            ::render::pipeline pipeline;

            struct target {
                ::render::attachments images;
                std::unique_ptr<::render::target> framebuffer;
            };

            std::vector<target> targets;

            std::array<std::string, 2> shaders;
        };

        state& get()
        {
            static state data;
            return data;
        }


        std::array<vk::DescriptorSetLayoutBinding,10> instance_bindings()
        {
            std::array<vk::DescriptorSetLayoutBinding, 10> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = (i < 4 || i == 9) ?
                    vk::DescriptorType::eStorageBuffer :
                    vk::DescriptorType::eCombinedImageSampler;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = i < 4 ?
                    vk::ShaderStageFlags(vk::ShaderStageFlagBits::eCompute) :
                    i == 9 ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eVertex) :
                    vk::ShaderStageFlags(vk::ShaderStageFlagBits::eFragment);
                if (i == 0)
                    bindings[i].stageFlags |= vk::ShaderStageFlagBits::eVertex;
            }


            return bindings;
        }

        // Shared render-system binding for streamed and local meshes.
        void draw_mesh(::render::renderer& graphics, const gpu_mesh& mesh,
                       const gpu_mesh::per_image& frame,
                       const draw_parameters& params, bool indirect,
                       std::uint32_t instances)
        {
            ::render::mesh geometry = mesh.geometry.view();
            geometry.streams.push_back({1,frame.visible.handle,0});

            ::render::material surface{};
            surface.pipeline = get().pipeline.handle();
            surface.layout = get().layout;
            surface.sets = {frame.set};
            surface.push_data = &params;
            surface.push_size = sizeof(params);
            surface.push_stages = vk::ShaderStageFlagBits::eVertex |
                                  vk::ShaderStageFlagBits::eFragment;

            if (indirect)
                graphics.draw_indirect(geometry, surface, frame.indirect.handle, 0);
            else
                graphics.draw(geometry, surface, instances);
        }

        draw_parameters material_parameters(const ::scene::material& m,
                                           const mat4& view_projection,
                                           std::uint32_t joints)
        {
            const auto flags =
                (m.base_color_texture ? 1u : 0u) |
                (m.metallic_roughness_texture ? 2u : 0u) |
                (m.normal_texture ? 4u : 0u) |
                (m.occlusion_texture ? 8u : 0u) |
                (m.emissive_texture ? 16u : 0u) |
                (m.alpha_mode << 5) |
                (m.procedural_pattern == 1u ? 128u : 0u);
            return {
                view_projection,m.base_color,
                {m.metallic,m.roughness,m.occlusion,m.alpha_cutoff},
                {m.emissive[0],m.emissive[1],m.emissive[2],float(flags)},
                joints
            };
        }

        void destroy_meshes(std::vector<gpu_mesh>& meshes)
        {
            for (auto& mesh : meshes)
            {
                mesh.geometry.reset();
                if (mesh.instance_buffer.handle)
                {
                    mesh.instance_buffer.reset();
                }
                if (mesh.bounds_buffer.handle)
                {
                    mesh.bounds_buffer.reset();
                }
                for (auto& frame : mesh.frames)
                {
                    if (frame.visible.handle) frame.visible.reset();
                    if (frame.indirect.handle) frame.indirect.reset();
                    if (frame.skin_palette.handle) frame.skin_palette.reset();
                    if (frame.local_instances.handle)
                    {
                        frame.local_instances.reset();
                    }
                }
            }

            meshes.clear();
        }

        struct plane {
            vec3 normal;
            float offset;
            float magnitude;
        };

        std::array<plane, 6> frustum(const mat4& m)
        {
            const auto make = [&](int row, float sign)
            {
                plane p{
                    {m(3, 0) + sign * m(row, 0),
                     m(3, 1) + sign * m(row, 1),
                     m(3, 2) + sign * m(row, 2)},
                    m(3, 3) + sign * m(row, 3), 0
                };
                p.magnitude = length(p.normal);
                return p;
            };
            plane near{
                {m(2, 0), m(2, 1), m(2, 2)}, m(2, 3), 0
            };
            near.magnitude = length(near.normal);
            return {make(0, 1), make(0, -1), make(1, 1), make(1, -1),
                    near, make(2, -1)};
        }

        void create_pipeline()
        {
            auto& data = get();
            vk::PushConstantRange range{};
            range.stageFlags = vk::ShaderStageFlagBits::eVertex |
                               vk::ShaderStageFlagBits::eFragment;
            range.size = sizeof(draw_parameters);
            vk::PipelineLayoutCreateInfo layout_info{};
            layout_info.pushConstantRangeCount = 1;
            layout_info.pPushConstantRanges = &range;
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &data.instance_layout;
            data.layout = vk::pipeline_layout::create(layout_info);

            ::render::raster_state raster{};
            raster.pass = data.render_pass;
            raster.layout = data.layout;
            raster.cull = vk::CullModeFlagBits::eNone;
            raster.front = vk::FrontFace::eCounterClockwise;
            raster.depth_compare = vk::CompareOp::eLess;
            raster.bindings = {
                {0, sizeof(::scene::vertex), vk::VertexInputRate::eVertex},
                {1, sizeof(std::uint32_t), vk::VertexInputRate::eInstance}
            };
            raster.attributes = {
                {0, 0, vk::Format::eR32G32B32Sfloat,
                 offsetof(::scene::vertex, position)},
                {1, 0, vk::Format::eR32G32B32Sfloat,
                 offsetof(::scene::vertex, normal)},
                {2, 0, vk::Format::eR32G32Sfloat,
                 offsetof(::scene::vertex, u)},
                {3, 1, vk::Format::eR32Uint, 0},
                {4, 0, vk::Format::eR32G32B32A32Uint,
                 offsetof(::scene::vertex, joints)},
                {5, 0, vk::Format::eR32G32B32A32Sfloat,
                 offsetof(::scene::vertex, weights)}
            };
            raster.blend_attachments.resize(3);
            for (auto& blend : raster.blend_attachments)
                blend.colorWriteMask = vk::ColorComponentFlagBits::eR |
                                       vk::ColorComponentFlagBits::eG |
                                       vk::ColorComponentFlagBits::eB |
                                       vk::ColorComponentFlagBits::eA;

            data.pipeline.create(raster, {
                {vk::ShaderStageFlagBits::eVertex, data.shaders[0]},
                {vk::ShaderStageFlagBits::eFragment, data.shaders[1]}
            });
        }

        std::array<vk::Format, 3> color_formats()
        {
            const auto precision = game::options::get()->graphics.gbuffer;
            const auto format = precision == game::options::gbuffer_precision::rgba16f
                ? vk::Format::eR16G16B16A16Sfloat : vk::Format::eR8G8B8A8Unorm;
            return {format, format, format};
        }

        vk::Format depth_format()
        {
            const auto precision = game::options::get()->graphics.depth;
            const auto format = precision == game::options::depth_precision::d32
                ? vk::Format::eD32Sfloat : vk::Format::eD16Unorm;
            const auto properties = vk::physical_device::get()->handle
                .getFormatProperties(format);
            const auto required = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                vk::FormatFeatureFlagBits::eSampledImage;
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("Configured depth format unsupported");
            return format;
        }

        void create_render_pass()
        {
            auto& data = get();
            std::array<vk::AttachmentDescription, 4> attachments{};
            std::array<vk::AttachmentReference, 3> colors{};

            for (std::uint32_t i = 0; i < colors.size(); ++i)
            {
                attachments[i].format = color_formats()[i];
                attachments[i].samples = vk::SampleCountFlagBits::e1;
                attachments[i].loadOp = vk::AttachmentLoadOp::eClear;
                attachments[i].storeOp = vk::AttachmentStoreOp::eStore;
                attachments[i].initialLayout = vk::ImageLayout::eUndefined;
                attachments[i].finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                colors[i] = {i, vk::ImageLayout::eColorAttachmentOptimal};
            }

            attachments[3].format = depth_format();
            attachments[3].samples = vk::SampleCountFlagBits::e1;
            attachments[3].loadOp = vk::AttachmentLoadOp::eClear;
            attachments[3].storeOp = vk::AttachmentStoreOp::eStore;
            attachments[3].initialLayout = vk::ImageLayout::eUndefined;
            attachments[3].finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;

            vk::AttachmentReference depth{3, vk::ImageLayout::eDepthStencilAttachmentOptimal};
            vk::SubpassDescription subpass{};
            subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
            subpass.colorAttachmentCount = colors.size();
            subpass.pColorAttachments = colors.data();
            subpass.pDepthStencilAttachment = &depth;

            std::array<vk::SubpassDependency, 2> barriers{};
            barriers[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            barriers[0].dstSubpass = 0;
            barriers[0].srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
            barriers[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eEarlyFragmentTests;
            barriers[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            barriers[1].srcSubpass = 0;
            barriers[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            barriers[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                vk::PipelineStageFlagBits::eLateFragmentTests;
            barriers[1].dstStageMask = vk::PipelineStageFlagBits::eComputeShader;
            barriers[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;

            vk::RenderPassCreateInfo info{};
            info.attachmentCount = attachments.size();
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = barriers.size();
            info.pDependencies = barriers.data();
            data.render_pass = vk::render_pass::create(info);
        }

        void create_target()
        {
            auto& data=get();
            const auto extent=vk::swapchain::get()->extent;
            const auto formats=color_formats();
            state::target target{};
            target.images.create(extent,
                std::vector<vk::Format>(formats.begin(),formats.end()),depth_format());
            target.framebuffer=std::make_unique<::render::target>();
            target.framebuffer->create_framebuffer(
                data.render_pass,
                {target.images.color(0),target.images.color(1),
                 target.images.color(2),target.images.depth()},
                extent);
            data.targets.push_back(std::move(target));
        }
    } // namespace

    void init(const char* vertex_path, const char* fragment_path)
    {
        if (!vertex_path || !fragment_path || !*vertex_path || !*fragment_path)
        {
            throw std::invalid_argument("Scene shader paths must not be empty");
        }

        auto& data = get();
        data.shaders = {vertex_path, fragment_path};

        const auto bindings=instance_bindings();
        const auto device = vk::device::get()->handle;
        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        data.instance_layout = device.createDescriptorSetLayout(info);
        texture::init();
        weapon::initialize();
        texture::pin(weapon::assets());
        texture::synchronize(::scene::state{});

        data.cull.create(game::options::get()->graphics.culling_shader,
                         data.instance_layout);
    }

    gbuffer outputs(std::uint32_t image)
    {
        const auto& target = get().targets.at(image);
        return {{
            target.images.color(0),target.images.color(1),
            target.images.color(2),target.images.depth()
        },vk::swapchain::get()->extent};
    }

    void release_local_frames()
    {
        auto& data = get();
        for (auto& mesh : data.local_meshes)
        {
            for (auto& frame : mesh.frames)
            {
                if (frame.local_instances.handle)
                {
                    frame.local_instances.reset();
                }
                if (frame.visible.handle)
                {
                    frame.visible.reset();
                }
                if (frame.skin_palette.handle)
                {
                    frame.skin_palette.reset();
                }
            }
            mesh.frames.clear();
        }
        data.local_sets.reset();
    }

    void create_local_frames()
    {
        auto& data = get();
        const auto& source = weapon::assets();
        if (source.meshes.empty())
        {
            return;
        }
        if (data.local_meshes.empty())
        {
            gpu::upload::batch transfers;
            for (const auto& mesh : source.meshes)
            {
                gpu_mesh output;
                output.id = mesh.id;
                output.skin_id = mesh.skin_id;
                output.joint_count = animation::joints(mesh.skin_id);
                output.surface = mesh.surface;
                output.index_count = mesh.indices.size();
                output.geometry.upload(transfers,
                    mesh.vertices.data(),
                    mesh.vertices.size() * sizeof(::scene::vertex),
                    mesh.indices.data(),
                    mesh.indices.size() * sizeof(std::uint32_t),
                    output.index_count);
                data.local_meshes.push_back(std::move(output));
            }
            transfers.finish();
        }
        const auto count=std::uint32_t(
            data.local_meshes.size()*vk::swapchain::get()->images.size());
        const auto schema=instance_bindings();
        const auto device=vk::device::get()->handle;
        data.local_sets=std::make_unique<gpu::descriptors>();
        data.local_sets->create(std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(),schema.end()),count);
        std::uint32_t descriptor_index=0;
        std::array<std::uint32_t, 192> identities{};
        for (std::uint32_t i = 0; i < identities.size(); ++i)
        {
            identities[i] = i;
        }
        for (auto& mesh : data.local_meshes)
        {
            mesh.frames.resize(vk::swapchain::get()->images.size());
            for (auto& frame : mesh.frames)
            {
                const auto memory = vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent;
                frame.local_instances = gpu::create_buffer(
                    192 * sizeof(instance_data), vk::BufferUsageFlagBits::eStorageBuffer, memory
                );
                frame.visible = gpu::create_buffer(
                    sizeof(identities),
                    vk::BufferUsageFlagBits::eVertexBuffer |
                        vk::BufferUsageFlagBits::eStorageBuffer,
                    memory
                );
                vk::buffer::write(frame.visible, identities.data(), sizeof(identities));
                frame.skin_palette = gpu::create_buffer(
                    std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix),
                    vk::BufferUsageFlagBits::eStorageBuffer,
                    memory
                );
                frame.set=data.local_sets->set(descriptor_index++);
                std::array<vk::DescriptorBufferInfo, 5> infos{};
                std::array<vk::WriteDescriptorSet, 5> writes{};
                for (std::uint32_t i = 0; i < 5; ++i)
                {
                    infos[i].buffer =
                        i == 4 ? frame.skin_palette.handle : frame.local_instances.handle;
                    infos[i].range = VK_WHOLE_SIZE;
                    writes[i].dstSet = frame.set;
                    writes[i].dstBinding = i == 4 ? 9 : i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                    writes[i].pBufferInfo = &infos[i];
                }
                device.updateDescriptorSets(writes, {});
                texture::bind(frame.set, mesh.surface);
            }
        }
    }

    void release_frame_resources()
    {
        auto& data = get();
        release_local_frames();
        // Keep immutable scene geometry across swapchain recreation.
        // Only the per-image culling, skinning and descriptor data changes.
        for(auto& mesh : data.meshes)
            mesh.frames.clear();
        data.instance_sets.reset();
        data.pipeline.reset();
        vk::pipeline_layout::destroy(data.layout);
        data.layout = nullptr;

        // Framebuffer handles must die before their attachment image views.
        data.targets.clear();
        vk::render_pass::destroy(data.render_pass);
        data.render_pass = nullptr;
        
    }

    void rebuild_instance_frames();

    void create_frame_resources()
    {
        try
        {
            create_render_pass();
            for (std::size_t i = 0; i < vk::swapchain::get()->images.size(); ++i)
            {
                create_target();
            }
            create_pipeline();
            create_local_frames();
            rebuild_instance_frames();
        }
        catch (...)
        {
            release_frame_resources();
            throw;
        }
    }

    void append_instance(gpu_mesh& mesh, const ::scene::instance& instance)
    {
        const auto& pose = instance.pose;
        const vec3 center{
            mesh.center.x * pose.scale.x,
            mesh.center.y * pose.scale.y,
            mesh.center.z * pose.scale.z
        };
        const vec3 inverse{
            pose.scale.x ? 1.0f / pose.scale.x : 0.0f,
            pose.scale.y ? 1.0f / pose.scale.y : 0.0f,
            pose.scale.z ? 1.0f / pose.scale.z : 0.0f
        };
        const float max_scale = std::max({
            std::abs(pose.scale.x), std::abs(pose.scale.y), std::abs(pose.scale.z)
        });
        mesh.instances.push_back(instance);
        mesh.transforms.push_back({
            trs(pose.position, pose.rotation, pose.scale),
            rotation(pose.rotation) * scale(inverse)
        });
        const auto position = pose.position + rotate(pose.rotation, center);
        mesh.bounds.push_back({
            position.x, position.y, position.z, mesh.radius * max_scale
        });
    }

    void bind_instances(gpu_mesh& mesh)
    {
        if (!mesh.instance_buffer.handle || !mesh.bounds_buffer.handle)
        {
            return;
        }

        for (auto& frame : mesh.frames)
        {
            if (!frame.set || !frame.visible.handle || !frame.indirect.handle)
            {
                continue;
            }
            const std::array<vk::Buffer, 4> buffers{
                mesh.instance_buffer.handle,
                mesh.bounds_buffer.handle,
                frame.visible.handle,
                frame.indirect.handle
            };
            std::array<vk::DescriptorBufferInfo, 4> infos{};
            std::array<vk::WriteDescriptorSet, 4> writes{};

            for (std::uint32_t i = 0; i < buffers.size(); ++i)
            {
                infos[i].buffer = buffers[i];
                infos[i].offset = 0;
                infos[i].range = VK_WHOLE_SIZE;
                writes[i].dstSet = frame.set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                writes[i].pBufferInfo = &infos[i];
            }
            vk::device::get()->handle.updateDescriptorSets(writes, {});
            vk::DescriptorBufferInfo palette_info{};
            palette_info.buffer = frame.skin_palette.handle;
            palette_info.range = VK_WHOLE_SIZE;
            vk::WriteDescriptorSet palette_write{};
            palette_write.dstSet = frame.set;
            palette_write.dstBinding = 9;
            palette_write.descriptorCount = 1;
            palette_write.descriptorType = vk::DescriptorType::eStorageBuffer;
            palette_write.pBufferInfo = &palette_info;
            vk::device::get()->handle.updateDescriptorSets({palette_write}, {});
            texture::bind(frame.set, mesh.surface);
        }
    }

    void refresh_instances(gpu_mesh& mesh, std::size_t first = 0,
                           gpu::upload::batch* shared_upload = nullptr)
    {
        if (mesh.transforms.empty())
        {
            return;
        }

        gpu::upload::batch local_upload;
        auto& transfers=shared_upload?*shared_upload:local_upload;
        const auto transform_bytes = mesh.transforms.size() * sizeof(instance_data);
        if (!mesh.instance_buffer.handle || mesh.instance_buffer.size < transform_bytes)
        {
            auto replacement = transfers.add(
                mesh.transforms.data(), transform_bytes,
                vk::BufferUsageFlagBits::eStorageBuffer,
                std::max<vk::DeviceSize>(transform_bytes, mesh.instance_buffer.size * 2)
            );
            if (mesh.instance_buffer.handle) mesh.instance_buffer.reset();
            mesh.instance_buffer = replacement;
        }
        else if (first < mesh.transforms.size())
        {
            transfers.write(
                mesh.instance_buffer, mesh.transforms.data() + first,
                (mesh.transforms.size() - first) * sizeof(instance_data),
                first * sizeof(instance_data)
            );
        }

        const auto visible_bytes = mesh.bounds.size() * sizeof(std::uint32_t);
        const auto count = vk::swapchain::get()->images.size();
        mesh.frames.resize(count);
        for (auto& frame : mesh.frames)
        {
            if (!frame.visible.handle || frame.visible.size < visible_bytes)
            {
                auto replacement = gpu::create_buffer(
                    std::max<vk::DeviceSize>(visible_bytes, frame.visible.size * 2),
                    vk::BufferUsageFlagBits::eVertexBuffer |
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                if (frame.visible.handle) frame.visible.reset();
                frame.visible = std::move(replacement);
            }

            if (!frame.indirect.handle)
            {
                frame.indirect = gpu::create_buffer(
                    sizeof(vk::DrawIndexedIndirectCommand),
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst |
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
            }
        }

        const auto palette_size = std::max<std::size_t>(
            sizeof(animation::matrix), mesh.instances.size() *
                std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix));
        for (auto& frame : mesh.frames)
        {
            if (!frame.skin_palette.handle || frame.skin_palette.size < palette_size)
            {
                if (frame.skin_palette.handle) frame.skin_palette.reset();
                frame.skin_palette = gpu::create_buffer(
                    palette_size, vk::BufferUsageFlagBits::eStorageBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent);
            }
        }
        const auto size = mesh.bounds.size() * sizeof(mesh.bounds.front());
        if (!mesh.bounds_buffer.handle || mesh.bounds_buffer.size < size)
        {
            auto replacement = transfers.add(
                mesh.bounds.data(), size, vk::BufferUsageFlagBits::eStorageBuffer,
                std::max<vk::DeviceSize>(size, mesh.bounds_buffer.size * 2)
            );
            if (mesh.bounds_buffer.handle) mesh.bounds_buffer.reset();
            mesh.bounds_buffer = replacement;
        }
        else if (first < mesh.bounds.size())
        {
            transfers.write(
                mesh.bounds_buffer, mesh.bounds.data() + first,
                (mesh.bounds.size() - first) * sizeof(mesh.bounds.front()),
                first * sizeof(mesh.bounds.front())
            );
        }

        if(!shared_upload)transfers.finish();
        bind_instances(mesh);
    }

    void rebuild_instance_frames()
    {
        auto& data = get();
        if(data.meshes.empty()) return;

        const auto count = std::max<std::size_t>(1, data.meshes.size()) *
            std::max<std::size_t>(1, vk::swapchain::get()->images.size());
        const auto schema = instance_bindings();
        data.instance_sets = std::make_unique<gpu::descriptors>();
        data.instance_sets->create(
            std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(), schema.end()),
            static_cast<std::uint32_t>(count));

        std::uint32_t index = 0;
        gpu::upload::batch transfers;
        for(auto& mesh : data.meshes)
        {
            if(mesh.transforms.empty()) continue;
            // Existing instance and geometry buffers remain GPU-resident.
            refresh_instances(mesh, mesh.transforms.size(),&transfers);
            for(auto& frame : mesh.frames)
                frame.set = data.instance_sets->set(index++);
            bind_instances(mesh);
        }
        transfers.finish();
    }

    bool needs_sync()
    {
        return connection::snapshot() != get().snapshot;
    }

    void synchronize()
    {
        auto& data = get();
        const auto source = connection::snapshot();

        if (source == data.snapshot)
        {
            return;
        }

        // Scene batches are append-only; reuse immutable GPU meshes and grow
        // only the buffers containing newly generated instances.
        bool appended = data.snapshot && source &&
            source->textures.size() >= data.snapshot->textures.size() &&
            source->meshes.size() == data.snapshot->meshes.size() &&
            source->instances.size() >= data.snapshot->instances.size();

        if (appended)
        {
            for (std::size_t i = 0; i < source->meshes.size(); ++i)
            {
                const auto& a = source->meshes[i];
                const auto& b = data.snapshot->meshes[i];
                if (!::scene::same_mesh(a, b))
                {
                    appended = false;
                    break;
                }
            }
        }

        if (appended)
        {
            appended = std::equal(
                data.snapshot->instances.begin(), data.snapshot->instances.end(),
                source->instances.begin(), ::scene::same_instance
            );
        }

        if (appended)
        {
            animation::synchronize(*source);
            if (source->textures.size() != data.snapshot->textures.size())
                texture::synchronize(*source);
            std::vector<std::size_t> previous_counts;
            previous_counts.reserve(data.meshes.size());
            for (const auto& mesh : data.meshes)
            {
                previous_counts.push_back(mesh.transforms.size());
            }

            std::unordered_map<std::uint32_t,std::size_t> mesh_indices;
            mesh_indices.reserve(data.meshes.size());
            for(std::size_t j=0;j<data.meshes.size();++j)
                mesh_indices.emplace(data.meshes[j].id,j);
            for(std::size_t i=data.snapshot->instances.size();
                i<source->instances.size();++i)
            {
                const auto found=mesh_indices.find(source->instances[i].mesh_id);
                if(found!=mesh_indices.end())
                    append_instance(data.meshes[found->second],source->instances[i]);
            }
            gpu::upload::batch scene_upload;
            for (std::size_t i = 0; i < data.meshes.size(); ++i)
            {
                if (data.meshes[i].transforms.size() != previous_counts[i])
                {
                    refresh_instances(data.meshes[i], previous_counts[i],&scene_upload);
                }
            }
            scene_upload.finish();
            data.snapshot = source;
            return;
        }

        std::vector<gpu_mesh> meshes;
        std::unique_ptr<gpu::descriptors> new_sets;

        if (source)
        {
            animation::synchronize(*source);
            texture::synchronize(*source);
        }
        else
        {
            animation::reset();
            texture::reset();
            texture::synchronize(::scene::state{});
        }

        if (source)
        {
            meshes.reserve(source->meshes.size());

            gpu::upload::batch transfers;
            try
            {
                for (const auto& mesh : source->meshes)
                {
                    gpu_mesh result;
                    result.id = mesh.id;
                    result.surface = mesh.surface;
                    result.skin_id = mesh.skin_id;
                    result.joint_count = animation::joints(mesh.skin_id);
                    result.index_count =
                        static_cast<std::uint32_t>(mesh.indices.size());

                    try
                    {
                        const auto vertex_bytes =
                            mesh.vertices.size() * sizeof(::scene::vertex);

                        const auto index_bytes =
                            mesh.indices.size() * sizeof(std::uint32_t);

                        result.geometry.upload(transfers,
                            mesh.vertices.data(),vertex_bytes,
                            mesh.indices.data(),index_bytes,result.index_count);

                        if (!mesh.vertices.empty())
                        {
                            vec3 low = mesh.vertices.front().position;
                            vec3 high = low;
                            for (const auto& vertex : mesh.vertices)
                            {
                                const auto p = vertex.position;
                                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                            }
                            result.center = (low + high) * 0.5f;
                            result.radius = length((high - low) * 0.5f);
                        }

                        meshes.push_back(std::move(result));
                    }
                    catch (...)
                    {
                        // The geometry owner releases both buffers on unwinding.
                        destroy_meshes(meshes);
                        throw;
                    }
                }
                transfers.finish();
            }
            catch (...)
            {
                destroy_meshes(meshes);
                throw;
            }
        }

        try
        {
            if (source)
            {
                std::unordered_map<std::uint32_t,std::size_t> mesh_indices;
                mesh_indices.reserve(meshes.size());
                for(std::size_t j=0;j<meshes.size();++j)
                    mesh_indices.emplace(meshes[j].id,j);
                for(const auto& instance:source->instances)
                {
                    const auto found=mesh_indices.find(instance.mesh_id);
                    if(found==mesh_indices.end())continue;
                    auto& mesh=meshes[found->second];
                    if(mesh.index_count)append_instance(mesh,instance);
                }

                // Allocate each scene descriptor through the reusable GPU
                // descriptor owner, using a layout compatible with the renderer.
                const auto count=std::max<std::size_t>(1,meshes.size()) *
                    std::max<std::size_t>(1,vk::swapchain::get()->images.size());
                const auto schema=instance_bindings();
                new_sets=std::make_unique<gpu::descriptors>();
                new_sets->create(std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(),schema.end()),
                    static_cast<std::uint32_t>(count));
                std::uint32_t descriptor_index=0;
                gpu::upload::batch instances_upload;

                for (auto& mesh : meshes)
                {
                    if (mesh.transforms.empty())
                    {
                        continue;
                    }

                    refresh_instances(mesh,0,&instances_upload);

                    for (auto& frame : mesh.frames)
                    {
                        frame.set=new_sets->set(descriptor_index++);
                    }
                    bind_instances(mesh);
                }
                instances_upload.finish();
            }
        }
        catch (...)
        {
            destroy_meshes(meshes);
            new_sets.reset();
            throw;
        }

        
        destroy_meshes(data.meshes);
        data.instance_sets=std::move(new_sets);
        data.meshes = std::move(meshes);
        data.snapshot = source;
        for (auto& mesh : data.local_meshes)
        {
            for (auto& frame : mesh.frames)
            {
                texture::bind(frame.set, mesh.surface);
            }
        }
    }

    void draw(vk::CommandBuffer command, std::uint32_t image,
              const camera::frame& camera)
    {
        auto& data = get();
        const auto& weapon_draws = weapon::update(camera.view);
        const auto extent = vk::swapchain::get()->extent;
        const float aspect = float(extent.width) / float(extent.height);
        const auto view_projection =
            perspective(camera.pose.fov, aspect, 0.1f, 1000.0f) * camera.view;
        const auto weapon_view_projection = weapon::view_projection(aspect, camera.view);

        std::array<vk::ClearValue, 4> clear{};
        for (std::size_t i = 0; i < 3; ++i)
        {
            clear[i].color = vk::ClearColorValue(
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
            );
        }
        clear[3].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

        vk::RenderPassBeginInfo begin{};
        begin.renderPass = data.render_pass;
        begin.framebuffer = data.targets.at(image).framebuffer->framebuffer();
        begin.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        begin.clearValueCount = clear.size();
        begin.pClearValues = clear.data();

        // Compute visibility and compact instance IDs for indirect graphics draws.
        if (data.snapshot)
        {
            const auto camera_planes = frustum(view_projection);
            cull_parameters params;
            for (std::size_t i = 0; i < params.planes.size(); ++i)
            {
                const auto& plane = camera_planes[i];
                const float inverse = 1.0f / std::max(plane.magnitude, 1e-6f);
                params.planes[i] = {
                    plane.normal.x * inverse,
                    plane.normal.y * inverse,
                    plane.normal.z * inverse,
                    plane.offset * inverse
                };
            }

            std::vector<vk::Buffer> indirects;
            indirects.reserve(data.meshes.size());
            for (const auto& mesh : data.meshes)
                if (!mesh.transforms.empty())
                    indirects.push_back(mesh.frames.at(image).indirect.handle);
            data.cull.begin(command, indirects);
            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty()) continue;
                data.cull.dispatch(
                    command, mesh.frames.at(image).set, params.planes,
                    static_cast<std::uint32_t>(mesh.transforms.size()),
                    mesh.index_count
                );
            }
            data.cull.end(command);


        }

        // Write per-swapchain-image palettes only after its frame fence completes.
        if (data.snapshot)
        {
            const auto server_tick = connection::get()->server_tick.load(
                std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (server_tick != data.last_animation_tick)
            {
                data.last_animation_tick = server_tick;
                data.animation_received = now;
            }
            const auto elapsed = std::chrono::duration<float>(
                now - data.animation_received).count();
            const float smooth_tick = float(server_tick) + std::clamp(elapsed, 0.f, 0.2f) * float(net::limits::tick_hz);
            for (auto& mesh : data.meshes)
            {
                if (!mesh.joint_count || mesh.transforms.empty()) continue;
                std::vector<animation::matrix> matrices;
                matrices.reserve(mesh.instances.size() * mesh.joint_count);
                // Multiple instances often use the same clip and start tick.
                // Sample that pose once per frame; retain each instance's
                // palette order and the same exact animation time.
                std::map<std::array<std::uint32_t,5>,
                         std::vector<animation::matrix>> sampled_poses;
                for (const auto& instance : mesh.instances)
                {
                    const auto update = connection::animation(instance.id);
                    const auto start = update ? update->start_tick : instance.animation_tick;
                    const auto clip = update ? update->clip : instance.animation;
                    const auto overlay = update ? update->overlay : instance.overlay_animation;
                    const auto weight = update ? update->blend : instance.overlay_weight;
                    const auto root = update ? update->mask_root : instance.overlay_root;
                    const std::array<std::uint32_t,5> key{
                        clip,start,overlay,root,std::bit_cast<std::uint32_t>(weight)
                    };
                    auto found=sampled_poses.find(key);
                    if(found==sampled_poses.end())
                    {
                        const float time=(smooth_tick-float(start))/
                            float(net::limits::tick_hz);
                        std::vector<animation::matrix> bones;
                        if(!animation::sample(mesh.skin_id,clip,std::max(0.f,time),
                            bones,overlay,weight,root) ||
                            bones.size()!=mesh.joint_count)
                            throw std::runtime_error("Cannot sample streamed animation");
                        found=sampled_poses.emplace(key,std::move(bones)).first;
                    }
                    const auto& bones=found->second;
                    matrices.insert(matrices.end(),bones.begin(),bones.end());
                }
                vk::buffer::write(mesh.frames.at(image).skin_palette,
                                  matrices.data(), matrices.size() * sizeof(matrices[0]));
            }
        }

        for (const auto& item : weapon_draws)
        {
            auto found = std::find_if(
                data.local_meshes.begin(),
                data.local_meshes.end(),
                [&](const gpu_mesh& mesh)
                {
                    return mesh.id == item.mesh_id;
                }
            );
            if (found == data.local_meshes.end())
            {
                continue;
            }
            auto& frame = found->frames.at(image);
            vk::buffer::write(
                frame.local_instances,
                item.instances.data(),
                item.instances.size() * sizeof(weapon::instance_data)
            );
            if (!item.palette.empty())
            {
                vk::buffer::write(
                    frame.skin_palette,
                    item.palette.data(),
                    item.palette.size() * sizeof(animation::matrix)
                );
            }
        }
        ::render::renderer graphics(command);
        graphics.begin_pass(begin);
        graphics.viewport(
            vk::Viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.1f, 1.0f},
            vk::Rect2D{{0, 0}, extent});

        if (data.snapshot)
        {
            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty())
                {
                    continue;
                }

                const auto& m = mesh.surface;
                const auto params=material_parameters(m,view_projection,mesh.joint_count);
                draw_mesh(graphics, mesh, mesh.frames.at(image), params, true, 1);
            }
        }

        // Disjoint depth ranges give the viewmodel independent depth testing while
        // preserving world depth for deferred HDR lighting and sky classification.
        // Draw both world-attached weapons and the first-person viewmodel.
        for (bool viewmodel : {false, true})
        {
            graphics.viewport(
                vk::Viewport{
                    0, 0, float(extent.width), float(extent.height),
                    viewmodel ? 0.f : .1f, viewmodel ? .099f : 1.f
                },
                vk::Rect2D{{0, 0}, extent});
            for (const auto& item : weapon_draws)
            {
                if (item.viewmodel != viewmodel)
                {
                    continue;
                }
                const auto found = std::find_if(
                    data.local_meshes.begin(),
                    data.local_meshes.end(),
                    [&](const gpu_mesh& mesh)
                    {
                        return mesh.id == item.mesh_id;
                    }
                );
                if (found == data.local_meshes.end())
                {
                    continue;
                }
                const auto& mesh = *found;
                const auto& frame = mesh.frames.at(image);
                const auto& m = mesh.surface;
                const auto params=material_parameters(m,
                    viewmodel?weapon_view_projection:view_projection,mesh.joint_count);
                draw_mesh(graphics, mesh, frame, params, false,
                          static_cast<std::uint32_t>(item.instances.size()));
            }
        }

        graphics.end_pass();
    }

    void shutdown()
    {
        auto& data = get();

        destroy_meshes(data.meshes);
        release_local_frames();
        destroy_meshes(data.local_meshes);
        weapon::shutdown();
        data.snapshot.reset();
        data.instance_sets.reset();

        release_frame_resources();

        data.cull.reset();
        texture::shutdown();
        if (data.instance_layout)
        {
            vk::device::get()->handle.destroyDescriptorSetLayout(data.instance_layout);
            data.instance_layout = nullptr;
        }
    }
} // namespace clt::scene_renderer



// [clt/render/texture.cpp]
// Flat per-process texture state. VulkanGPU owns all texture residency and hashes.
#define STBI_NO_STDIO
#include <gpu/upload.hpp>
#include <gpu/texture_cache.hpp>
#include <stb_image.h>
#include <array>
#include <climits>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace clt::texture {
namespace {
    gpu::texture_cache cache;
    vk::Sampler sampler{};
    std::vector<::scene::texture> pinned;
    std::unordered_set<std::uint32_t> live;

    std::uint32_t checksum(const std::vector<std::uint8_t>& bytes) {
        std::uint32_t hash=2166136261u;
        for(auto b:bytes)hash=(hash^b)*16777619u;
        return hash;
    }

    void upload(const ::scene::texture& texture,gpu::upload::batch& batch) {
        const auto& bytes=texture.encoded.empty()?texture.pixels:texture.encoded;
        const auto hash=checksum(bytes);
        if(cache.contains(texture.id,hash))return;
        std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> decoded(nullptr,stbi_image_free);
        const stbi_uc* pixels=texture.pixels.data();
        if(texture.pixels.empty()) {
            int width=0,height=0,channels=0;
            if(texture.encoded.empty()||texture.encoded.size()>INT_MAX)
                throw std::runtime_error("Missing local texture data");
            decoded.reset(stbi_load_from_memory(texture.encoded.data(),
                int(texture.encoded.size()),&width,&height,&channels,4));
            if(!decoded||width!=int(texture.width)||height!=int(texture.height))
                throw std::runtime_error("Invalid local texture dimensions");
            pixels=decoded.get();
        } else if(!texture.width||!texture.height||
            texture.pixels.size()!=std::size_t(texture.width)*texture.height*4u)
            throw std::runtime_error("Invalid texture pixel count");
        cache.upsert(texture.id,hash,pixels,texture.width,texture.height,batch);
    }
}

void init() {
    if(sampler)throw std::runtime_error("Texture system already initialized");
    vk::SamplerCreateInfo info{};
    info.magFilter=info.minFilter=vk::Filter::eLinear;
    info.mipmapMode=vk::SamplerMipmapMode::eLinear;
    info.addressModeU=info.addressModeV=info.addressModeW=vk::SamplerAddressMode::eRepeat;
    sampler=vk::device::get()->handle.createSampler(info);
    try {
        gpu::upload::batch batch;
        ::scene::texture white{};
        white.id=0;white.width=white.height=1;
        white.pixels={255,255,255,255};
        upload(white,batch);
        batch.finish();
    } catch(...) {shutdown();throw;}
}

void pin(const ::scene::state& scene) {pinned=scene.textures;}

void synchronize(const ::scene::state& scene) {
    gpu::upload::batch batch;
    std::unordered_set<std::uint32_t> updated;
    updated.reserve(scene.textures.size()+pinned.size());
    std::vector<std::uint64_t> keep{0};
    keep.reserve(scene.textures.size()+pinned.size()+1);
    const auto process=[&](const ::scene::texture& texture) {
        if(!updated.insert(texture.id).second)return;
        upload(texture,batch);
        keep.push_back(texture.id);
    };
    for(const auto& texture:scene.textures)process(texture);
    for(const auto& texture:pinned)process(texture);
    batch.finish();
    cache.retain(keep);
    live=std::move(updated);
}

void bind(vk::DescriptorSet set,const ::scene::material& material) {
    const std::array<std::uint32_t,5> ids{
        material.base_color_texture,material.metallic_roughness_texture,
        material.normal_texture,material.occlusion_texture,material.emissive_texture};
    std::array<vk::DescriptorImageInfo,5> images{};
    std::array<vk::WriteDescriptorSet,5> writes{};
    for(std::size_t i=0;i<ids.size();++i) {
        images[i].sampler=sampler;
        images[i].imageView=cache.view(live.contains(ids[i])?ids[i]:0u);
        images[i].imageLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
        writes[i].dstSet=set;
        writes[i].dstBinding=std::uint32_t(4+i);
        writes[i].descriptorCount=1;
        writes[i].descriptorType=vk::DescriptorType::eCombinedImageSampler;
        writes[i].pImageInfo=&images[i];
    }
    vk::device::get()->handle.updateDescriptorSets(writes,{});
}

void reset() {
    live.clear();
    cache.retain({0});
}
void shutdown() {
    reset();
    pinned.clear();
    cache.clear();
    if(sampler)vk::device::get()->handle.destroySampler(sampler);
    sampler=nullptr;
}
} // namespace clt::texture

// [clt/view/camera.cpp]





#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace clt::camera
{
    using namespace vk::util::math;
    namespace
    {
        std::mutex camera_mutex;
        float recoil = 0, aim_fraction = 0, ads_blend = 0;
        float ads_sensitivity_multiplier=1.f;
        std::chrono::steady_clock::time_point recoil_time{};

        float current_recoil()
        {
            return recoil *
                   std::exp(
                       -12.f *
                       std::chrono::duration<float>(std::chrono::steady_clock::now() - recoil_time)
                           .count()
                   );
        }

        vec3 eye_position()
        {
            const auto parameters = connection::movement_parameters();
            return prediction::position() + vec3{0, parameters ? parameters->height : 0.0f, 0};
        }
    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        *get() = {};
        recoil = aim_fraction = ads_blend = 0;
        ads_sensitivity_multiplier=1.f;
        recoil_time = std::chrono::steady_clock::now();
        get()->fov = 1.04719755f;
        get()->sensitivity = 0.0025f;
    }

    void set_look_sensitivity(float base,float ads_multiplier)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        get()->sensitivity=std::clamp(base,.00001f,.1f);
        ads_sensitivity_multiplier=std::clamp(ads_multiplier,.05f,1.f);
    }

    void set_ads_fraction(float fraction)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        ads_blend=std::clamp(fraction,0.f,1.f);
    }

    void look(float dx, float dy)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        auto& camera = *get();
        const float sensitivity=camera.sensitivity*
            (1.f-ads_blend*(1.f-ads_sensitivity_multiplier));

        camera.yaw = std::remainder(
            camera.yaw + dx * sensitivity,
            6.283185307f
        );

        camera.pitch = std::clamp(
            camera.pitch - dy * sensitivity,
            -1.55f,
            1.55f
        );
    }

    state snapshot()
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        auto result = *get();
        result.pitch = std::clamp(result.pitch + current_recoil(), -1.55f, 1.55f);
        result.fov *= 1.f - aim_fraction;
        return result;
    }

    void kick(float radians)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        recoil = std::min(current_recoil() + radians, .08f);
        recoil_time = std::chrono::steady_clock::now();
    }

    void aim(float fraction)
    {
        std::lock_guard<std::mutex> lock(camera_mutex);
        aim_fraction = std::clamp(fraction, 0.f, .95f);
    }

    frame capture()
    {
        const auto camera = snapshot();
        const vec3 eye = eye_position();
        const vec3 direction{
            std::sin(camera.yaw) * std::cos(camera.pitch),
            std::sin(camera.pitch),
            -std::cos(camera.yaw) * std::cos(camera.pitch)
        };
        return {camera, look_at(eye, eye + direction), eye};
    }

    mat4 view()
    {
        return capture().view;
    }

    mat4 projection(float aspect)
    {
        return perspective(snapshot().fov, aspect, 0.1f, 1000.0f);
    }

    mat4 view_projection(float aspect)
    {
        const auto frame = capture();
        return perspective(frame.pose.fov, aspect, 0.1f, 1000.0f) * frame.view;
    }
} // namespace clt::camera



// [clt/weapon/weapon.cpp]






// [srv/assets/model.hpp]



#include <cstdint>
#include <filesystem>

namespace srv::assets
{
    // CPU-only glTF import. The caller publishes the resulting scene on the
    // authoritative server thread; no renderer or Vulkan objects are involved.
    ::scene::state load(const std::filesystem::path& path,
                        std::uint32_t first_mesh,
                        std::uint32_t first_instance,
                        std::uint32_t first_texture,
                        std::uint32_t first_skin);
} // namespace srv::assets



#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace clt::weapon
{
    using namespace vk::util::math;

    namespace
    {
        using clock = std::chrono::steady_clock;
        constexpr std::uint32_t asset_id = 0x60000000;
        constexpr std::uint32_t hole_id = asset_id + 0x10000;
        constexpr std::uint32_t flash_id = hole_id + 1;

        struct intent
        {
            ::weapon::action action;
            float yaw, pitch;
            std::uint32_t weapon_id = 0;
        };

        std::mutex input_mutex;
        std::deque<intent> intents;
        bool trigger_held = false, reload_held = false, aim = false;
        bool magazine_check_requested = false;
        std::uint32_t movement = 0;
        bindings selected_input{};
        double selected_fire_interval=.15;
        clock::time_point next_automatic_fire{};
        std::vector<equip_binding> registered_equips;

        struct casing
        {
            vec3 position, velocity;
            quat orientation;
            float age = 0, spin = 0;
            bool sleeping = false;
            std::uint32_t weapon_id = 0;
            float lifetime = 0;
        };

        struct hole
        {
            mat4 transform;
            float age = 0;
            float lifetime = 0;
        };

        struct resource
        {
            std::uint32_t skin = 0;
            std::vector<std::uint32_t> meshes;
        };

        struct slot
        {
            definition settings;
            resource model, casing;
            std::map<motion, std::uint32_t> clips;
            ::weapon::state authority, predicted;
            double next_fire = 0;
        };

        struct runtime
        {
            ::scene::state asset;
            std::vector<slot> weapons;
            std::size_t active = 0;
            std::uint32_t equip_sequence = 0;
            std::vector<draw> draws;
            std::vector<casing> casings;
            std::deque<hole> holes;
            ::weapon::state predicted;
            std::deque<std::pair<net::weapon::command, double>> pending;
            std::unordered_set<std::uint32_t> confirmed;
            std::shared_ptr<const ::scene::state> collision_source;
            ::weapon::world collision;
            clock::time_point started{}, previous{};
            mat4 camera_correction = mat4::identity();
            std::uint32_t skin = 0, camera_joint = UINT32_MAX, muzzle = UINT32_MAX,
                          ejection = UINT32_MAX, slide = UINT32_MAX;
            std::uint32_t clip = 0, sequence = 0, shot_variant = 0;
            double clip_started = 0, flash_until = 0;
            std::uint32_t last_fired_sequence = 0;
            float speed = 1, ads = 0, recoil = 0;
            bool loop = false, ready = false, connected = false, presented_aim = false;
            mat4 muzzle_matrix = mat4::identity();
        };

        runtime& get()
        {
            static runtime value;
            return value;
        }

        slot& current()
        {
            return get().weapons.at(get().active);
        }

        const definition& config()
        {
            return current().settings;
        }

        auto find_weapon(std::uint32_t id)
        {
            auto& weapons = get().weapons;
            return std::find_if(
                weapons.begin(),
                weapons.end(),
                [id](const slot& item)
                {
                    return item.settings.rules.id == id;
                }
            );
        }

        bool contains(const resource& asset, std::uint32_t mesh)
        {
            return std::find(asset.meshes.begin(), asset.meshes.end(), mesh) != asset.meshes.end();
        }

        double seconds()
        {
            return std::chrono::duration<double>(clock::now() - get().started).count();
        }

        mat4 inverse_affine(mat4 m)
        {
            const vec3 a{m(0, 0), m(1, 0), m(2, 0)}, b{m(0, 1), m(1, 1), m(2, 1)},
                c{m(0, 2), m(1, 2), m(2, 2)};
            const float determinant = dot(a, cross(b, c));
            if (std::abs(determinant) < 1e-10f)
            {
                throw std::runtime_error("Singular weapon socket transform");
            }
            const auto r0 = cross(b, c) / determinant, r1 = cross(c, a) / determinant,
                       r2 = cross(a, b) / determinant;
            auto out = mat4::identity();
            for (int i = 0; i < 3; ++i)
            {
                out(0, i) = (&r0.x)[i];
                out(1, i) = (&r1.x)[i];
                out(2, i) = (&r2.x)[i];
            }
            const vec3 p{m(0, 3), m(1, 3), m(2, 3)};
            out(0, 3) = -dot(r0, p);
            out(1, 3) = -dot(r1, p);
            out(2, 3) = -dot(r2, p);
            return out;
        }

        quat orientation(mat4 matrix)
        {
            const vec3 x = normalize(vec3{matrix(0, 0), matrix(1, 0), matrix(2, 0)});
            const vec3 y = normalize(vec3{matrix(0, 1), matrix(1, 1), matrix(2, 1)});
            const vec3 z = normalize(cross(x, y));
            mat4 m = mat4::identity();
            for (int r = 0; r < 3; ++r)
            {
                m(r, 0) = (&x.x)[r];
                m(r, 1) = (&y.x)[r];
                m(r, 2) = (&z.x)[r];
            }
            const float trace = m(0, 0) + m(1, 1) + m(2, 2);
            quat q;
            if (trace > 0)
            {
                const float k = 2 * std::sqrt(trace + 1);
                q = {
                    (m(2, 1) - m(1, 2)) / k,
                    (m(0, 2) - m(2, 0)) / k,
                    (m(1, 0) - m(0, 1)) / k,
                    k * .25f
                };
            }
            else
            {
                int a = 0;
                if (m(1, 1) > m(a, a))
                {
                    a = 1;
                }
                if (m(2, 2) > m(a, a))
                {
                    a = 2;
                }
                const int b = (a + 1) % 3, c = (a + 2) % 3;
                const float k = 2 * std::sqrt(1 + m(a, a) - m(b, b) - m(c, c));
                float v[4]{};
                v[a] = k * .25f;
                v[b] = (m(b, a) + m(a, b)) / k;
                v[c] = (m(c, a) + m(a, c)) / k;
                v[3] = (m(c, b) - m(b, c)) / k;
                q = {v[0], v[1], v[2], v[3]};
            }
            return normalize(q);
        }

        instance_data instance(mat4 model)
        {
            return {model, transpose(inverse_affine(model))};
        }

        std::uint32_t clip_index(motion name)
        {
            return get().skin?current().clips.at(name):0u;
        }

        void play(motion name, double now)
        {
            auto& d = get();
            d.clip_started=now;
            if(!d.skin)
            {
                d.clip=0;
                d.speed=1;
                d.loop=false;
                return;
            }
            d.clip=clip_index(name);
            d.speed=config().animations.at(name).speed;
            d.loop=config().animations.at(name).loop;
        }

        void select(std::size_t index, double now)
        {
            auto& d = get();
            current().predicted = d.predicted;
            d.active = index;
            d.predicted = current().predicted;
            {
                std::lock_guard<std::mutex> lock(input_mutex);
                selected_input=config().input;
                selected_fire_interval=config().rules.fire_interval;
                next_automatic_fire={};
                trigger_held=false;
            }
            camera::set_look_sensitivity(config().input.sensitivity,
                                         config().input.ads_sensitivity_multiplier);
            d.skin=current().model.skin;
            d.camera_joint=d.muzzle=d.ejection=d.slide=UINT32_MAX;
            d.camera_correction=config().correction;
            if(d.skin)
            {
                d.camera_joint=animation::joint(d.skin,config().camera_joint);
                d.muzzle=animation::joint(d.skin,config().muzzle_joint);
                d.ejection=animation::joint(d.skin,config().ejection_joint);
                d.slide=animation::joint(d.skin,config().slide_joint);
                std::vector<animation::matrix> palette;
                if(!animation::sample(d.skin,clip_index(motion::idle),0,palette))
                    throw std::runtime_error("Cannot sample weapon reference pose");
                animation::matrix reference{};
                if(d.camera_joint!=UINT32_MAX &&
                   animation::joint_transform(d.skin,d.camera_joint,reference))
                    d.camera_correction=
                        inverse_affine(mat4{reference})*config().correction;
            }
            d.presented_aim = false;
            d.ads = 0;
            camera::aim(0);
            camera::set_ads_fraction(0);
            d.recoil = 0;
            d.flash_until = 0;
            play(motion::draw, now);
        }

        bool finished(double now)
        {
            const auto& d = get();
            return !d.skin ||
                   (!d.loop &&
                    (now-d.clip_started)*d.speed>=animation::duration(d.skin,d.clip));
        }

        mat4 socket(std::uint32_t index)
        {
            animation::matrix matrix{};
            return animation::joint_transform(get().skin, index, matrix) ? mat4{matrix}
                                                                         : mat4::identity();
        }

        void append(::scene::state source)
        {
            auto& target = get().asset;
            target.meshes.insert(
                target.meshes.end(),
                std::make_move_iterator(source.meshes.begin()),
                std::make_move_iterator(source.meshes.end())
            );
            target.textures.insert(
                target.textures.end(),
                std::make_move_iterator(source.textures.begin()),
                std::make_move_iterator(source.textures.end())
            );
            target.instances.insert(
                target.instances.end(),
                std::make_move_iterator(source.instances.begin()),
                std::make_move_iterator(source.instances.end())
            );
            target.skins.insert(
                target.skins.end(),
                std::make_move_iterator(source.skins.begin()),
                std::make_move_iterator(source.skins.end())
            );
        }

        ::scene::mesh disc(std::uint32_t id, bool flash)
        {
            ::scene::mesh mesh;
            mesh.id = id;
            mesh.surface.base_color = flash ? std::array<float, 4>{1, .7f, .18f, 1}
                                            : std::array<float, 4>{.018f, .014f, .012f, 1};
            mesh.surface.roughness = .95f;
            if (flash)
            {
                mesh.surface.emissive = {8, 5, 1};
            }
            constexpr int points = 16;
            mesh.vertices.push_back({{0, 0, 0}, {0, 0, 1}, .5f, .5f});
            for (int i = 0; i < points; ++i)
            {
                const float a = float(i) * 6.2831853f / points;
                const float radius = flash ? (i % 2 ? .25f : 1.f) : (i % 3 ? 1.f : .86f);
                mesh.vertices.push_back(
                    {{std::cos(a) * radius, std::sin(a) * radius, 0}, {0, 0, 1}, 0, 0}
                );
                mesh.indices.insert(
                    mesh.indices.end(),
                    {0u, std::uint32_t(i + 1), std::uint32_t((i + 1) % points + 1)}
                );
            }
            return mesh;
        }

        ::weapon::hit trace_casing(vec3 from, vec3 to)
        {
            ::weapon::hit result;
            const auto delta = to - from;
            const spatial::bounds bounds{
                {std::min(from.x, to.x) - .004f,
                 std::min(from.y, to.y) - .004f,
                 std::min(from.z, to.z) - .004f},
                {std::max(from.x, to.x) + .004f,
                 std::max(from.y, to.y) + .004f,
                 std::max(from.z, to.z) + .004f}
            };
            float nearest = 1.2f;
            const auto& world = get().collision;
            world.index.query(
                bounds,
                [&](std::uint32_t index)
                {
                    const auto& t = world.triangles[index];
                    const auto e1 = t.b - t.a, e2 = t.c - t.a, p = cross(delta, e2);
                    const float det = dot(e1, p);
                    if (std::abs(det) < 1e-12f)
                    {
                        return;
                    }
                    const auto s = from - t.a;
                    const float u = dot(s, p) / det;
                    if (u < 0 || u > 1)
                    {
                        return;
                    }
                    const auto q = cross(s, e1);
                    const float v = dot(delta, q) / det;
                    if (v < 0 || u + v > 1)
                    {
                        return;
                    }
                    const float ratio = dot(e2, q) / det;
                    if (ratio < 0 || ratio >= nearest || ratio > 1.f)
                    {
                        return;
                    }
                    auto normal = normalize(cross(e1, e2));
                    if (dot(normal, delta) > 0)
                    {
                        normal = normal * -1.f;
                    }
                    nearest = ratio;
                    result = {true, from + delta * ratio, normal, t.instance, t.index};
                }
            );
            return result;
        }

        void add_impact(const ::weapon::hit& impact, const definition& settings)
        {
            if (!impact.impacted)
            {
                return;
            }
            auto n = normalize(impact.normal);
            auto x = normalize(cross(std::abs(n.y) < .95f ? vec3{0, 1, 0} : vec3{1, 0, 0}, n));
            auto y = cross(n, x);
            auto m = mat4::identity();
            const float size = settings.impact_radius;
            for (int r = 0; r < 3; ++r)
            {
                m(r, 0) = (&x.x)[r] * size;
                m(r, 1) = (&y.x)[r] * size;
                m(r, 2) = (&n.x)[r] * size;
            }
            auto p = impact.position + n * settings.impact_offset;
            m(0, 3) = p.x;
            m(1, 3) = p.y;
            m(2, 3) = p.z;
            auto& d = get();
            if (d.holes.size() >= settings.max_impacts)
            {
                d.holes.pop_front();
            }
            d.holes.push_back({m, 0, settings.impact_lifetime});
        }
    } // namespace

    bindings active_bindings()
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        return selected_input;
    }

    std::vector<equip_binding> equip_bindings()
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        return registered_equips;
    }

    void controls(bool trigger, bool ads, bool reload, std::uint32_t buttons)
    {
        const auto camera = camera::snapshot();
        std::lock_guard<std::mutex> lock(input_mutex);
        const auto now=clock::now();
        if (trigger && intents.size() < 64 &&
            (!trigger_held ||
             (selected_input.automatic && now>=next_automatic_fire)))
        {
            intents.push_back({::weapon::action::fire, camera.yaw, camera.pitch});
            next_automatic_fire=now+
                std::chrono::duration_cast<clock::duration>(
                    std::chrono::duration<double>(selected_fire_interval));
        }
        if (reload && !reload_held && intents.size() < 64)
        {
            intents.push_back({::weapon::action::reload, camera.yaw, camera.pitch});
        }
        trigger_held = trigger;
        if(!trigger)next_automatic_fire={};
        reload_held = reload;
        aim = ads;
        movement = buttons;
    }

    void register_weapon(definition value)
    {
        auto& d = get();
        if (d.ready)
        {
            throw std::logic_error("Register weapons before renderer initialization");
        }
        ::weapon::validate(value.rules);
        if (value.model.empty() ||
            find_weapon(value.rules.id) != d.weapons.end() || value.max_casings > 192 ||
            value.max_casings == 0 || value.max_impacts == 0 || value.max_impacts > 192 ||
            value.near_plane <= 0 || value.far_plane <= value.near_plane || value.fov <= 0 ||
            value.fov >= 3.14159265f || value.ads_fov <= 0 || value.ads_fov >= 3.14159265f ||
            !std::isfinite(value.input.sensitivity) || value.input.sensitivity<=0.f ||
            value.input.sensitivity>.1f || !std::isfinite(value.input.ads_sensitivity_multiplier) ||
            value.input.ads_sensitivity_multiplier<=0.f ||
            value.input.ads_sensitivity_multiplier>1.f)
        {
            throw std::invalid_argument("Invalid client weapon definition");
        }
        // Only idle is mandatory. Additional clips inherit a compatible
        // motion, so a gun with one fire clip does not need 15 identical
        // animation declarations.
        if(!value.animations.empty() && !value.animations.count(motion::idle))
            throw std::invalid_argument("Animated gun requires an idle clip");
        auto inherit=[&](motion target,motion source,bool loop)
        {
            if(value.animations.empty() || value.animations.count(target))return;
            auto clip=value.animations.at(source);
            clip.loop=loop;
            value.animations.emplace(target,std::move(clip));
        };
        inherit(motion::walk,motion::idle,true);
        inherit(motion::draw,motion::idle,false);
        inherit(motion::reload,motion::idle,false);
        inherit(motion::reload_empty,motion::reload,false);
        inherit(motion::ads_idle,motion::idle,true);
        inherit(motion::ads_walk,motion::walk,true);
        inherit(motion::ads_in,motion::ads_idle,false);
        inherit(motion::ads_out,motion::idle,false);
        inherit(motion::fire1,motion::idle,false);
        inherit(motion::fire2,motion::fire1,false);
        inherit(motion::fire3,motion::fire1,false);
        inherit(motion::ads_fire1,motion::fire1,false);
        inherit(motion::ads_fire2,motion::ads_fire1,false);
        inherit(motion::ads_fire3,motion::ads_fire1,false);
        for(const auto& [motion,clip] : value.animations)
        {
            if(clip.name.empty() || !std::isfinite(clip.speed) || clip.speed<=0)
                throw std::invalid_argument("Invalid weapon animation clip");
        }
        const auto id=value.rules.id;
        const auto key=value.input.equip_key;
        if(key>=0)
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            for(const auto& registered:registered_equips)
                if(registered.key==key)
                    throw std::invalid_argument("Duplicate weapon equip key");
        }
        slot item;
        item.settings=std::move(value);
        item.authority=item.predicted=::weapon::make_state(item.settings.rules);
        d.weapons.push_back(std::move(item));
        if(key>=0)
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            registered_equips.push_back({id,key});
        }
    }

    void equip(std::uint32_t id)
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        if (id && intents.size() < 64)
        {
            intents.push_back({::weapon::action::equip, 0, 0, id});
        }
    }

    void check_magazine()
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        magazine_check_requested = true;
    }

    void initialize()
    {
        auto& d = get();
        if (d.ready)
        {
            return;
        }
        if (d.weapons.empty())
        {
            d.ready = true;
            return;
        }
        d.started = d.previous = clock::now();
        std::map<std::filesystem::path, resource> cache;
        std::uint32_t next_id = asset_id;
        auto load = [&](const std::filesystem::path& path)
        {
            const auto key = path.lexically_normal();
            if (const auto found = cache.find(key); found != cache.end())
            {
                return found->second;
            }
            const auto started = clock::now();
            auto imported = srv::assets::load(path, next_id, next_id, next_id, next_id);
            if (imported.meshes.empty())
            {
                throw std::runtime_error("Weapon model has no meshes: " + path.string());
            }
            resource result;
            for (const auto& mesh : imported.meshes)
            {
                result.meshes.push_back(mesh.id);
            }
            if (!imported.skins.empty())
            {
                result.skin = imported.skins.front().id;
            }
            if (imported.skins.size() > 1)
            {
                throw std::runtime_error(
                    "Viewmodel meshes must share one skeleton: " + path.string()
                );
            }
            next_id += std::uint32_t(
                           std::max(
                               {imported.meshes.size(),
                                imported.instances.size(),
                                imported.textures.size(),
                                imported.skins.size()}
                           )
                       ) +
                       1;
            if (next_id >= hole_id)
            {
                throw std::runtime_error("Local model resource ID budget exceeded");
            }
            std::cout << "clt: imported local model " << path.string() << " ("
                      << imported.meshes.size() << " meshes, " << imported.textures.size()
                      << " textures, "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             clock::now() - started
                         )
                             .count()
                      << " ms)\n";
            append(std::move(imported));
            cache.emplace(key, result);
            return result;
        };
        for (auto& item : d.weapons)
        {
            item.model = load(item.settings.model);
            if(!item.settings.casing_model.empty())
                item.casing=load(item.settings.casing_model);
            if(item.casing.skin || (item.model.skin && item.settings.animations.empty()) ||
               (!item.model.skin && !item.settings.animations.empty()))
                throw std::runtime_error(
                    "Skinned guns need animation clips; static guns omit them; casing must be static");
            const auto skin = std::find_if(
                d.asset.skins.begin(),
                d.asset.skins.end(),
                [&](const ::scene::skin& entry)
                {
                    return entry.id == item.model.skin;
                }
            );
            for (const auto& [motion, configured] : item.settings.animations)
            {
                const auto found = std::find_if(
                    skin->clips.begin(),
                    skin->clips.end(),
                    [&](const ::scene::clip& entry)
                    {
                        return entry.name == configured.name;
                    }
                );
                if (found == skin->clips.end())
                {
                    throw std::runtime_error("Missing weapon animation: " + configured.name);
                }
                item.clips.emplace(motion, std::uint32_t(found - skin->clips.begin()));
            }
        }
        animation::synchronize(d.asset);
        d.asset.meshes.push_back(disc(hole_id, false));
        d.asset.meshes.push_back(disc(flash_id, true));
        d.predicted = current().predicted;
        select(0, seconds());
        d.casings.reserve(192);
        d.ready = true;
    }

    const ::scene::state& assets()
    {
        return get().asset;
    }

    const std::vector<draw>& update(const mat4& view)
    {
        auto& d = get();
        if (d.weapons.empty())
        {
            return d.draws;
        }
        const auto now = seconds();
        const auto time = clock::now();
        const float dt =
            std::clamp(std::chrono::duration<float>(time - d.previous).count(), 0.f, .05f);
        d.previous = time;
        animation::synchronize(d.asset);
        bool aiming;
        std::uint32_t moving;
        std::deque<intent> input;
        bool inspect_magazine=false;
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            aiming = aim;
            moving = movement;
            input.swap(intents);
            inspect_magazine=magazine_check_requested;
            magazine_check_requested=false;
        }
        const bool connected = connection::get()->connected.load(std::memory_order_relaxed);
        current().predicted = d.predicted;
        if (connected != d.connected)
        {
            for (auto& item : d.weapons)
            {
                item.authority = item.predicted = ::weapon::make_state(item.settings.rules);
                item.next_fire = 0;
            }
            d.predicted = current().predicted;
            d.pending.clear();
            d.confirmed.clear();
            d.sequence = d.equip_sequence = 0;
            d.last_fired_sequence = 0;
            d.casings.clear();
            d.holes.clear();
            d.connected = connected;
            select(0, now);
        }
        for (const auto& update : connection::take_weapon_updates())
        {
            const auto found = find_weapon(update.weapon_id);
            if (found == d.weapons.end())
            {
                throw std::runtime_error("Server weapon ID is not registered on this client");
            }
            auto& item = *found;
            if (update.magazine > item.settings.rules.magazine_capacity ||
                update.reserve > item.settings.rules.initial_reserve)
            {
                continue;
            }
            if (update.outcome == ::weapon::result::fired &&
                net::message::newer(update.sequence, d.last_fired_sequence))
            {
                d.last_fired_sequence = update.sequence;
                const auto command = std::find_if(
                    d.pending.begin(),
                    d.pending.end(),
                    [&](const auto& p)
                    {
                        return p.first.sequence == update.sequence;
                    }
                );
                item.next_fire = (command == d.pending.end() ? now : command->second) +
                                 item.settings.rules.fire_interval;
            }
            std::erase_if(
                d.pending,
                [&](const auto& pending)
                {
                    return !net::message::newer(pending.first.sequence, update.sequence) &&
                           (pending.first.kind == ::weapon::action::equip ||
                            pending.first.weapon_id == update.weapon_id);
                }
            );
            item.authority.magazine = update.magazine;
            item.authority.reserve = update.reserve;
            item.authority.reload = update.reload;
            item.authority.sequence = update.sequence;
            item.authority.next_fire = item.next_fire;
            if (update.reload != ::weapon::reload_phase::none)
            {
                item.authority.reload_started = now - update.reload_elapsed;
                item.authority.reload_finishes =
                    item.authority.reload_started + update.reload_duration;
                item.authority.refill_at = item.authority.reload_started + update.refill_time;
                item.authority.refilled = update.reload_elapsed >= update.refill_time;
            }
            for (auto& owned : d.weapons)
            {
                owned.predicted = owned.authority;
            }
            for (const auto& pending : d.pending)
            {
                const auto target = find_weapon(pending.first.weapon_id);
                if (target != d.weapons.end() && pending.first.kind != ::weapon::action::equip)
                {
                    ::weapon::apply(
                        target->predicted,
                        pending.first.sequence,
                        pending.first.kind,
                        pending.second,
                        target->settings.rules
                    );
                }
            }
            for (auto& owned : d.weapons)
            {
                ::weapon::advance(owned.predicted, now, owned.settings.rules);
            }
            d.predicted = current().predicted;
            if (!net::message::newer(d.equip_sequence, update.sequence) &&
                update.active_weapon_id != config().rules.id)
            {
                const auto equipped = find_weapon(update.active_weapon_id);
                if (equipped == d.weapons.end())
                {
                    throw std::runtime_error("Server equipped an unregistered weapon ID");
                }
                select(std::size_t(equipped - d.weapons.begin()), now);
            }
            if (d.predicted.reload != ::weapon::reload_phase::none)
            {
                const auto reload_clip = clip_index(
                    d.predicted.reload == ::weapon::reload_phase::empty ? motion::reload_empty
                                                                        : motion::reload
                );
                if (d.clip != reload_clip)
                {
                    play(
                        d.predicted.reload == ::weapon::reload_phase::empty ? motion::reload_empty
                                                                            : motion::reload,
                        d.predicted.reload_started
                    );
                    d.presented_aim = false;
                }
            }
            else if (
                d.clip == clip_index(motion::reload) || d.clip == clip_index(motion::reload_empty)
            )
            {
                play(motion::idle, now);
                d.presented_aim = false;
            }
        }
        const auto source = connection::snapshot();
        for (const auto& shot : connection::take_weapon_shots())
        {
            if (d.confirmed.insert(shot.state.sequence).second)
            {
                const auto owner = find_weapon(shot.state.weapon_id);
                if (owner != d.weapons.end())
                {
                    add_impact(shot.impact, owner->settings);
                }
            }
        }
        if (d.confirmed.size() > 1024)
        {
            d.confirmed.clear();
        }
        ::weapon::advance(d.predicted, now, config().rules);
        if(inspect_magazine)
        {
            std::cout << "magazine: " << d.predicted.magazine << "/"
                      << config().rules.magazine_capacity
                      << " | reserve: " << d.predicted.reserve;
            if(d.predicted.reload != ::weapon::reload_phase::none)
                std::cout << " | reloading: "
                          << std::max(0.,d.predicted.reload_finishes-now) << "s";
            std::cout << std::endl;
        }
        bool fired = false;
        for (const auto& event : input)
        {
            if (!connected)
            {
                continue;
            }
            const auto requested =
                event.action == ::weapon::action::equip ? event.weapon_id : config().rules.id;
            const auto target = find_weapon(requested);
            if (target == d.weapons.end() ||
                (event.action == ::weapon::action::equip && requested == config().rules.id))
            {
                continue;
            }
            // Do not flood the network with fire attempts during an empty
            // magazine or reload. Keep authoritative checks on the server.
            if(event.action == ::weapon::action::fire &&
               (d.predicted.magazine == 0 ||
                d.predicted.reload != ::weapon::reload_phase::none))
                continue;
            if(event.action == ::weapon::action::reload &&
               (d.predicted.magazine >= config().rules.magazine_capacity ||
                d.predicted.reserve == 0 ||
                d.predicted.reload != ::weapon::reload_phase::none))
                continue;
            net::weapon::command command;
            command.sequence = d.sequence + 1;
            if (!command.sequence)
            {
                ++command.sequence;
            }
            command.weapon_id = requested;
            command.kind = event.action;
            command.yaw = event.yaw;
            command.pitch = event.pitch;
            if (!connection::send_weapon(command))
            {
                continue;
            }
            d.sequence = command.sequence;
            d.pending.push_back({command, now});
            if (command.kind == ::weapon::action::equip)
            {
                select(std::size_t(target - d.weapons.begin()), now);
                fired = false;
                d.equip_sequence = command.sequence;
                continue;
            }
            const auto outcome =
                ::weapon::apply(d.predicted, command.sequence, command.kind, now, config().rules);
            if (outcome == ::weapon::result::fired)
            {
                const auto variant = ++d.shot_variant % 3;
                play(motion(int(aiming ? motion::ads_fire1 : motion::fire1) + int(variant)), now);
                d.presented_aim = aiming;
                d.flash_until = now + config().flash_duration;
                d.recoil = std::min(d.recoil + config().recoil, config().recoil * 3);
                camera::kick(aiming ? config().ads_kick : config().hip_kick);
                fired = true;
            }
            else if (outcome == ::weapon::result::reload_started)
            {
                play(
                    d.predicted.reload == ::weapon::reload_phase::empty ? motion::reload_empty
                                                                        : motion::reload,
                    now
                );
                d.presented_aim = false;
            }
        }
        const auto idle_motion = aiming ? (moving ? motion::ads_walk : motion::ads_idle)
                                        : (moving ? motion::walk : motion::idle);
        const auto idle = clip_index(idle_motion);
        auto idle_or_transition = [&]
        {
            if (aiming != d.presented_aim)
            {
                play(aiming ? motion::ads_in : motion::ads_out, now);
                d.presented_aim = aiming;
            }
            else
            {
                play(idle_motion, now);
            }
        };
        if (finished(now))
        {
            idle_or_transition();
        }
        if (d.loop && (d.clip != idle || aiming != d.presented_aim))
        {
            idle_or_transition();
        }
        d.ads += (float(aiming) - d.ads) * (1 - std::exp(-config().ads_speed * dt));
        d.recoil *= std::exp(-config().recoil_decay * dt);
        camera::aim(d.ads * config().ads_zoom);
        camera::set_ads_fraction(d.ads);
        std::vector<animation::matrix> palette;
        if (d.skin && !animation::sample(
                d.skin,
                d.clip,
                float(now - d.clip_started) * d.speed,
                palette,
                UINT32_MAX,
                0,
                UINT32_MAX,
                d.loop
            ))
        {
            throw std::runtime_error("Cannot sample weapon animation");
        }
        if (d.skin && d.slide != UINT32_MAX &&
            d.predicted.magazine == 0 &&
            d.predicted.reload == ::weapon::reload_phase::none)
        {
            // Keep the last round's authored fire pose during its recoil, then hold the slide open.
            const bool firing =
                d.clip == clip_index(motion::fire1) || d.clip == clip_index(motion::fire2) ||
                d.clip == clip_index(motion::fire3) || d.clip == clip_index(motion::ads_fire1) ||
                d.clip == clip_index(motion::ads_fire2) || d.clip == clip_index(motion::ads_fire3);
            if (!firing || finished(now))
            {
                const auto backward =
                    inverse_affine(d.camera_correction) * vec4{0, 0, config().slide_offset, 0};
                animation::offset_joint(
                    d.skin, d.slide, {backward.x, backward.y, backward.z}, palette
                );
            }
        }
        const auto camera_world = inverse_affine(view);
        auto local = translate({0, 0, d.recoil}) * rotation(angle_axis(d.recoil, {1, 0, 0})) *
                     d.camera_correction;
        const auto model = camera_world * local;
        d.muzzle_matrix = model *
            (d.muzzle==UINT32_MAX?
             translate(config().muzzle_offset):socket(d.muzzle));
        if (fired && !config().casing_model.empty())
        {
            auto port = model *
                (d.ejection==UINT32_MAX?
                 translate(config().ejection_offset):socket(d.ejection));
            const auto position = port * vec4{0, 0, 0, 1};
            const auto impulse = config().casing_impulse;
            const auto velocity = camera_world * vec4{impulse.x, impulse.y, impulse.z, 0};
            if (d.casings.size() >= config().max_casings)
            {
                d.casings.erase(d.casings.begin());
            }
            d.casings.push_back(
                {{position.x, position.y, position.z},
                 {velocity.x, velocity.y, velocity.z},
                 orientation(port),
                 0,
                 config().casing_spin,
                 false,
                 config().rules.id,
                 config().casing_lifetime}
            );
        }
        if (source != d.collision_source)
        {
            bool append_only = d.collision_source && source &&
                               source->meshes.size() == d.collision_source->meshes.size() &&
                               source->instances.size() >= d.collision_source->instances.size();
            if (source)
            {
                if (!append_only)
                {
                    d.collision = {};
                }
                const auto first = append_only ? d.collision_source->instances.size() : 0;
                std::vector<::scene::instance> added(
                    source->instances.begin() + first, source->instances.end()
                );
                d.collision.append(source->meshes, added);
            }
            else
            {
                d.collision = {};
            }
            d.collision_source = source;
        }
        for (auto& c : d.casings)
        {
            c.age += dt;
            if (c.sleeping)
            {
                continue;
            }
            const int steps = std::max(1, int(std::ceil(dt / .008f)));
            const float h = dt / steps;
            for (int step = 0; step < steps; ++step)
            {
                c.velocity.y -= 9.81f * h;
                const auto displacement = c.velocity * h;
                const float distance = length(displacement);
                if (distance < 1e-7f)
                {
                    continue;
                }
                auto hit = trace_casing(c.position, c.position + displacement);
                if (hit.impacted && length(hit.position - c.position) <= distance + .004f)
                {
                    c.position = hit.position + hit.normal * .004f;
                    const float inward = dot(c.velocity, hit.normal);
                    if (inward < 0)
                    {
                        c.velocity = (c.velocity - hit.normal * (1.35f * inward)) * .65f;
                    }
                    c.spin *= .65f;
                    if (length(c.velocity) < .16f && hit.normal.y > .5f)
                    {
                        c.sleeping = true;
                    }
                }
                else
                {
                    c.position = c.position + displacement;
                }
                c.orientation = angle_axis(c.spin * h, {.3f, .7f, .2f}) * c.orientation;
            }
        }
        std::erase_if(
            d.casings,
            [](const casing& c)
            {
                return c.age > c.lifetime || c.position.y < -100;
            }
        );
        for (auto& h : d.holes)
        {
            h.age += dt;
        }
        std::erase_if(
            d.holes,
            [](const hole& h)
            {
                return h.age > h.lifetime;
            }
        );
        d.draws.clear();
        if (!connected)
        {
            return d.draws;
        }
        for (const auto& mesh : d.asset.meshes)
        {
            draw item;
            item.mesh_id = mesh.id;
            if (mesh.id == hole_id)
            {
                for (const auto& h : d.holes)
                {
                    item.instances.push_back(instance(h.transform));
                }
            }
            else if (mesh.id == flash_id)
            {
                item.viewmodel = true;
                if (now < d.flash_until)
                {
                    const auto p = d.muzzle_matrix * vec4{0, 0, 0, 1};
                    auto flash = camera_world;
                    flash(0, 3) = p.x;
                    flash(1, 3) = p.y;
                    flash(2, 3) = p.z;
                    item.instances.push_back(instance(
                        flash * rotation(angle_axis(float(d.shot_variant) * 2.4f, {0, 0, 1})) *
                        scale(config().flash_scale)
                    ));
                }
            }
            else if (contains(current().model, mesh.id))
            {
                item.viewmodel = true;
                item.palette = palette;
                for (const auto& node : d.asset.instances)
                {
                    if (node.mesh_id == mesh.id)
                    {
                        item.instances.push_back(instance(
                            model * trs(node.pose.position, node.pose.rotation, node.pose.scale)
                        ));
                    }
                }
            }
            else
            {
                for (const auto& c : d.casings)
                {
                    const auto owner = find_weapon(c.weapon_id);
                    if (owner != d.weapons.end() && contains(owner->casing, mesh.id))
                    {
                        item.instances.push_back(
                            instance(trs(c.position, c.orientation, {1, 1, 1}))
                        );
                    }
                }
            }
            if (!item.instances.empty())
            {
                d.draws.push_back(std::move(item));
            }
        }
        current().predicted = d.predicted;
        return d.draws;
    }

    mat4 view_projection(float aspect, const mat4& view)
    {
        if (get().weapons.empty())
        {
            return view;
        }
        const auto& settings = config();
        return perspective(
                   settings.fov + (settings.ads_fov - settings.fov) * get().ads,
                   aspect,
                   settings.near_plane,
                   settings.far_plane
               ) *
               view;
    }

    status inspect()
    {
        const auto& d = get();
        if (d.weapons.empty())
        {
            return {};
        }
        return {
            d.predicted.magazine,
            d.predicted.reserve,
            d.predicted.reload != ::weapon::reload_phase::none,
            d.ads,
            d.ready ? [&]() { for (const auto& skin : d.asset.skins){ if (skin.id == d.skin){
                return skin.clips.at(d.clip).name;
}} return std::string{}; }() : std::string{},
            d.casings.size(),
            d.holes.size(),
            d.ready ? config().rules.id : 0
        };
    }

    void shutdown()
    {
        get() = runtime{};
        std::lock_guard<std::mutex> lock(input_mutex);
        intents.clear();
        trigger_held = reload_held = aim = false;
        selected_input={};
        registered_equips.clear();
        next_automatic_fire={};
        movement = 0;
    }
} // namespace clt::weapon



// [srv/assets/image.cpp]

// [srv/assets/image.hpp]



#include <fastgltf/types.hpp>
#include <cstdint>
#include <vector>

namespace srv::assets
{
    // Validate the source image on the server and transmit its original PNG/JPEG
    // bytes. The client decodes it to RGBA8 before uploading to the GPU.
    std::vector<::scene::texture> images(const fastgltf::Asset& asset,
                                          std::uint32_t first_texture);
} // namespace srv::assets




#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#include <fastgltf/tools.hpp>

#include <climits>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>

namespace srv::assets
{
    namespace
    {
        std::span<const std::byte> encoded(const fastgltf::Asset& asset,
                                            const fastgltf::Image& image)
        {
            if (const auto* array = std::get_if<fastgltf::sources::Array>(&image.data))
                return {array->bytes.data(), array->bytes.size()};

            if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&image.data))
            {
                const auto& region = asset.bufferViews.at(view->bufferViewIndex);
                const auto& buffer = asset.buffers.at(region.bufferIndex);
                if (const auto* array = std::get_if<fastgltf::sources::Array>(&buffer.data))
                {
                    if (region.byteOffset > array->bytes.size() ||
                        region.byteLength > array->bytes.size() - region.byteOffset)
                        throw std::runtime_error("Invalid glTF image buffer view");
                    return {array->bytes.data() + region.byteOffset, region.byteLength};
                }
            }
            throw std::runtime_error("Unsupported glTF image source");
        }
    } // namespace

    std::vector<::scene::texture> images(const fastgltf::Asset& asset,
                                          std::uint32_t first_texture)
    {
        std::vector<::scene::texture> result;
        result.reserve(asset.images.size());
        for (std::size_t i = 0; i < asset.images.size(); ++i)
        {
            const auto source = encoded(asset, asset.images[i]);
            if (source.empty() || source.size() > INT_MAX)
                throw std::runtime_error("Invalid glTF image size");
            int w = 0, h = 0, channels = 0;
            auto* pixels = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(source.data()),
                int(source.size()), &w, &h, &channels, 4);
            if (!pixels || w <= 0 || h <= 0 ||
                std::uint64_t(w) * h * 4 > net::scene_stream::max_scene_bytes - 128)
            {
                stbi_image_free(pixels);
                throw std::runtime_error("Invalid or oversized glTF texture");
            }

            ::scene::texture texture;
            texture.id = first_texture + std::uint32_t(i);
            texture.width = std::uint32_t(w);
            texture.height = std::uint32_t(h);
            texture.encoded.assign(reinterpret_cast<const std::uint8_t*>(source.data()),
                                   reinterpret_cast<const std::uint8_t*>(source.data()) + source.size());
            stbi_image_free(pixels);
            result.push_back(std::move(texture));
        }
        return result;
    }
} // namespace srv::assets



#undef STB_IMAGE_IMPLEMENTATION

// [srv/assets/model.cpp]



// [srv/assets/rig.hpp]

#include <fastgltf/types.hpp>


#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace srv::assets
{
    // A validated Ozz skeleton and clips imported from one glTF skin.
    // GLB conversion happens once on the asset worker, not on gameplay ticks.
    struct rig
    {
        ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
        std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;
        std::vector<std::string> names;
        std::vector<std::uint32_t> skin_joints; // glTF skin order -> Ozz order
    };

    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index);
    ::scene::skin serialize_rig(const fastgltf::Asset& asset,
                                std::size_t skin_index, std::uint32_t id);
    ::scene::skin serialize_rig(
        const fastgltf::Asset& asset, std::size_t skin_index, std::uint32_t id, const rig& imported
    );
} // namespace srv::assets



#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace srv::assets
{
    namespace
    {
        ::scene::material material(const fastgltf::Asset& asset,
                                   const fastgltf::Primitive& primitive, std::uint32_t first_texture)
        {
            ::scene::material result;
            if (!primitive.materialIndex) return result;
            const auto& source = asset.materials.at(*primitive.materialIndex);
            for (int i = 0; i < 4; ++i)
                result.base_color[i] = float(source.pbrData.baseColorFactor[i]);
            for (int i = 0; i < 3; ++i)
                result.emissive[i] = float(source.emissiveFactor[i]);
            result.metallic = float(source.pbrData.metallicFactor);
            result.roughness = std::max(0.045f, float(source.pbrData.roughnessFactor));
            result.alpha_cutoff = float(source.alphaCutoff);
            result.alpha_mode = std::uint32_t(source.alphaMode);
            result.double_sided = source.doubleSided;
            const auto id = [&](std::size_t texture_index) -> std::uint32_t
            {
                const auto& ref = asset.textures.at(texture_index);
                if (!ref.imageIndex)
                    throw std::runtime_error("Unsupported compressed glTF texture");
                return first_texture + std::uint32_t(*ref.imageIndex);
            };
            if (source.pbrData.baseColorTexture)
                result.base_color_texture = id(source.pbrData.baseColorTexture->textureIndex);
            if (source.pbrData.metallicRoughnessTexture)
                result.metallic_roughness_texture = id(
                    source.pbrData.metallicRoughnessTexture->textureIndex);
            if (source.normalTexture)
                result.normal_texture = id(source.normalTexture->textureIndex);
            if (source.occlusionTexture)
                result.occlusion_texture = id(source.occlusionTexture->textureIndex);
            if (source.emissiveTexture)
                result.emissive_texture = id(source.emissiveTexture->textureIndex);
            return result;
        }

        vk::util::math::vec3 position(const fastgltf::math::nmat4x4& m,
                                      fastgltf::math::fvec3 p)
        {
            const float x = p[0], y = p[1], z = p[2];
            return {
                float(m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0]),
                float(m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1]),
                float(m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2])
            };
        }

        vk::util::math::vec3 normal(const fastgltf::math::nmat4x4& m,
                                    fastgltf::math::fvec3 n)
        {
            // Transform normals with the inverse transpose, including mirrored scale.
            using vec3 = vk::util::math::vec3;
            const vec3 a{float(m[0][0]), float(m[0][1]), float(m[0][2])};
            const vec3 b{float(m[1][0]), float(m[1][1]), float(m[1][2])};
            const vec3 c{float(m[2][0]), float(m[2][1]), float(m[2][2])};
            const auto x = cross(b, c);
            const auto y = cross(c, a);
            const auto z = cross(a, b);
            auto transformed = (x * n[0] + y * n[1] + z * n[2]) *
                (dot(a, x) < 0.0f ? -1.0f : 1.0f);
            const float d = length(transformed);
            return d > 1e-8f ? transformed * (1.f / d) : vec3{0, 1, 0};
        }
    } // namespace

    ::scene::state load(const std::filesystem::path& path,
                        std::uint32_t first_mesh, std::uint32_t first_instance,
                        std::uint32_t first_texture, std::uint32_t first_skin)
    {
        auto bytes = fastgltf::MappedGltfFile::FromPath(path);
        if (!bytes) throw std::runtime_error("Could not open glTF: " + path.string());

        fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization);
        constexpr auto options = fastgltf::Options::LoadExternalBuffers |
            fastgltf::Options::LoadExternalImages |
            fastgltf::Options::GenerateMeshIndices |
            fastgltf::Options::DecomposeNodeMatrices;

        auto parsed = parser.loadGltf(bytes.get(), path.parent_path(), options);
        if (parsed.error() != fastgltf::Error::None)
            throw std::runtime_error(std::string("Invalid glTF: ") +
                                     std::string(fastgltf::getErrorMessage(parsed.error())));

        auto asset = std::move(parsed.get());
        ::scene::state result;
        result.textures = images(asset, first_texture);
        std::vector<std::vector<std::uint32_t>> remaps;
        remaps.reserve(asset.skins.size());
        for (std::size_t i = 0; i < asset.skins.size(); ++i)
        {
            auto imported = import_rig(asset, i);
            result.skins.push_back(
                serialize_rig(asset, i, first_skin + std::uint32_t(i), imported)
            );
            remaps.push_back(std::move(imported.skin_joints));
        }
        if (asset.scenes.empty()) return result;

        const auto scene_index = asset.defaultScene.value_or(std::size_t(0));
        if (scene_index >= asset.scenes.size())
            throw std::runtime_error("Invalid glTF default scene");

        fastgltf::iterateSceneNodes(asset, scene_index, fastgltf::math::nmat4x4{},
            [&](fastgltf::Node& node, const fastgltf::math::nmat4x4& world)
        {
            if (!node.meshIndex || !node.visible) return;
            const bool skinned = node.skinIndex.has_value();
            const auto& source_mesh = asset.meshes.at(*node.meshIndex);
            for (const auto& primitive : source_mesh.primitives)
            {
                if (primitive.type != fastgltf::PrimitiveType::Triangles)
                    continue;
                const auto p = primitive.findAttribute("POSITION");
                if (p == primitive.attributes.end() || !primitive.indicesAccessor)
                    throw std::runtime_error("Missing indexed triangle positions");

                const auto& positions = asset.accessors.at(p->accessorIndex);
                const auto n = primitive.findAttribute("NORMAL");
                const auto u = primitive.findAttribute("TEXCOORD_0");
                const auto joints = primitive.findAttribute("JOINTS_0");
                const auto weights = primitive.findAttribute("WEIGHTS_0");
                if (skinned && (joints == primitive.attributes.end() ||
                                weights == primitive.attributes.end()))
                    throw std::runtime_error("Skin requires JOINTS_0 and WEIGHTS_0");

                ::scene::mesh mesh;
                mesh.id = first_mesh++;
                mesh.skin_id = skinned ? first_skin + std::uint32_t(*node.skinIndex) : 0;
                mesh.surface = material(asset, primitive, first_texture);
                mesh.vertices.resize(positions.count);

                for (std::size_t i = 0; i < positions.count; ++i)
                {
                    auto& vertex = mesh.vertices[i];
                    const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                        asset, positions, i);
                    vertex.position = skinned ? vk::util::math::vec3{p[0], p[1], p[2]} :
                        position(world, p);
                    if (n != primitive.attributes.end())
                    {
                        const auto normal_value = fastgltf::getAccessorElement<
                            fastgltf::math::fvec3>(asset, asset.accessors[n->accessorIndex], i);
                        vertex.normal = skinned ?
                            vk::util::math::vec3{normal_value[0], normal_value[1],
                                                 normal_value[2]} :
                            normal(world, normal_value);
                    }
                    else
                        vertex.normal = {0, 1, 0};
                    if (u != primitive.attributes.end())
                    {
                        const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>(
                            asset, asset.accessors[u->accessorIndex], i);
                        vertex.u = uv[0];
                        vertex.v = uv[1];
                    }
                    if (skinned)
                    {
                        const auto indices = fastgltf::getAccessorElement<
                            fastgltf::math::u16vec4>(
                            asset, asset.accessors[joints->accessorIndex], i);
                        const auto w = fastgltf::getAccessorElement<fastgltf::math::fvec4>(
                            asset, asset.accessors[weights->accessorIndex], i);
                        const auto& remap = remaps.at(*node.skinIndex);
                        float total = 0;
                        for (int k = 0; k < 4; ++k)
                        {
                            if (indices[k] >= remap.size())
                                throw std::runtime_error("Invalid skin joint index");
                            vertex.joints[k] = remap[indices[k]];
                            vertex.weights[k] = std::max(0.0f, w[k]);
                            total += vertex.weights[k];
                        }
                        if (total <= 0.0f) throw std::runtime_error("Zero skin weights");
                        for (float& weight : vertex.weights) weight /= total;
                    }
                }

                const auto& indices = asset.accessors.at(*primitive.indicesAccessor);
                mesh.indices.resize(indices.count);
                for (std::size_t i = 0; i < indices.count; ++i)
                    mesh.indices[i] = fastgltf::getAccessorElement<std::uint32_t>(asset, indices, i);

                if (mesh.indices.empty() || mesh.indices.size() % 3)
                    throw std::runtime_error("Invalid triangle index count");

                if (n == primitive.attributes.end())
                {
                    for (auto& vertex : mesh.vertices)
                        vertex.normal = {0, 0, 0};
                    for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
                    {
                        const auto a = mesh.indices[i];
                        const auto b = mesh.indices[i + 1];
                        const auto c = mesh.indices[i + 2];
                        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() ||
                            c >= mesh.vertices.size())
                            throw std::runtime_error("Invalid glTF mesh indices");
                        const auto face = cross(
                            mesh.vertices[b].position - mesh.vertices[a].position,
                            mesh.vertices[c].position - mesh.vertices[a].position
                        );
                        mesh.vertices[a].normal = mesh.vertices[a].normal + face;
                        mesh.vertices[b].normal = mesh.vertices[b].normal + face;
                        mesh.vertices[c].normal = mesh.vertices[c].normal + face;
                    }
                    for (auto& vertex : mesh.vertices)
                    {
                        const auto length_squared = dot(vertex.normal, vertex.normal);
                        vertex.normal = length_squared > 1e-12f ?
                            vertex.normal * (1.0f / std::sqrt(length_squared)) :
                            vk::util::math::vec3{0, 1, 0};
                    }
                }

                // Some model parts have no usable TEXCOORD_0. Assign a
                // deterministic planar chart so impacts can leave localized
                // UV stains instead of painting every fragment at (0,0).
                float low_u=std::numeric_limits<float>::infinity();
                float low_v=low_u;
                float high_u=-low_u,high_v=-low_u;
                for(const auto& vertex:mesh.vertices)
                {
                    low_u=std::min(low_u,vertex.u);
                    low_v=std::min(low_v,vertex.v);
                    high_u=std::max(high_u,vertex.u);
                    high_v=std::max(high_v,vertex.v);
                }
                if(u==primitive.attributes.end() ||
                   (high_u-low_u<1e-6f && high_v-low_v<1e-6f))
                {
                    using vec3=vk::util::math::vec3;
                    const auto component=[](vec3 p,int axis)
                    {
                        return axis==0?p.x:(axis==1?p.y:p.z);
                    };
                    std::array<float,3> lo{
                        mesh.vertices.front().position.x,
                        mesh.vertices.front().position.y,
                        mesh.vertices.front().position.z};
                    auto hi=lo;
                    for(const auto& vertex:mesh.vertices)
                        for(int axis=0;axis<3;++axis)
                        {
                            const auto value=component(vertex.position,axis);
                            lo[axis]=std::min(lo[axis],value);
                            hi[axis]=std::max(hi[axis],value);
                        }
                    std::array<int,3> axes{0,1,2};
                    std::sort(axes.begin(),axes.end(),[&](int a,int b)
                    {
                        return hi[a]-lo[a]>hi[b]-lo[b];
                    });
                    for(auto& vertex:mesh.vertices)
                    {
                        vertex.u=(component(vertex.position,axes[0])-lo[axes[0]])/
                            std::max(hi[axes[0]]-lo[axes[0]],1e-6f)*.98f+.01f;
                        vertex.v=(component(vertex.position,axes[1])-lo[axes[1]])/
                            std::max(hi[axes[1]]-lo[axes[1]],1e-6f)*.98f+.01f;
                    }
                }

                ::scene::instance instance;
                instance.id = first_instance++;
                instance.mesh_id = mesh.id;
                instance.animation = 0;
                instance.animation_tick = 0;
                result.meshes.push_back(std::move(mesh));
                result.instances.push_back(instance);
            }
        });
        return result;
    }
} // namespace srv::assets



// [srv/assets/rig.cpp]


#include <fastgltf/tools.hpp>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace srv::assets
{
    namespace
    {
        using raw_joint = ozz::animation::offline::RawSkeleton::Joint;
        using raw_clip = ozz::animation::offline::RawAnimation;
        constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

        ozz::math::Transform transform(const fastgltf::Node& node)
        {
            const auto* trs = std::get_if<fastgltf::TRS>(&node.transform);
            if (!trs) throw std::runtime_error("glTF node matrix must be decomposed");
            ozz::math::Transform value{};
            value.translation = {float(trs->translation[0]), float(trs->translation[1]),
                                 float(trs->translation[2])};
            value.rotation = {float(trs->rotation[0]), float(trs->rotation[1]),
                              float(trs->rotation[2]), float(trs->rotation[3])};
            value.scale = {float(trs->scale[0]), float(trs->scale[1]),
                           float(trs->scale[2])};
            return value;
        }
    } // namespace

    namespace
    {
        template<typename T>
        std::vector<std::uint8_t> archive(const T& value)
        {
            ozz::io::MemoryStream buffer;
            {
                ozz::io::OArchive writer(&buffer);
                writer << value;
            }
            std::vector<std::uint8_t> bytes(buffer.Size());
            if (buffer.Seek(0, ozz::io::Stream::kSet) ||
                buffer.Read(bytes.data(), bytes.size()) != bytes.size())
                throw std::runtime_error("Ozz archive could not be serialized");
            return bytes;
        }
    } // namespace

    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index)
    {
        const auto& skin = asset.skins.at(skin_index);
        const auto count = asset.nodes.size();
        std::vector<std::size_t> parent(count, none);
        for (std::size_t i = 0; i < count; ++i)
            for (const auto child : asset.nodes[i].children)
            {
                if (child >= count || parent[child] != none)
                    throw std::runtime_error("Invalid glTF joint hierarchy");
                parent[child] = i;
            }

        std::vector<bool> included(count, false);
        for (const auto joint : skin.joints)
        {
            if (joint >= count) throw std::runtime_error("Invalid skin joint index");
            auto node = joint;
            for (std::size_t steps = 0; node != none && steps <= count; ++steps)
            {
                included[node] = true;
                node = parent[node];
                if (steps == count)
                    throw std::runtime_error("Cyclic glTF joint hierarchy");
            }
        }

        ozz::animation::offline::RawSkeleton raw;
        std::vector<std::size_t> ordered;
        auto append = [&](std::size_t index, auto&& self) -> raw_joint
        {
            raw_joint joint{};
            const auto& name = asset.nodes[index].name;
            joint.name = name.empty() ? ("node_" + std::to_string(index)).c_str() :
                name.c_str();
            joint.transform = transform(asset.nodes[index]);
            ordered.push_back(index);
            for (auto child : asset.nodes[index].children)
                if (included[child]) joint.children.push_back(self(child, self));
            return joint;
        };
        for (std::size_t i = 0; i < count; ++i)
            if (included[i] && (parent[i] == none || !included[parent[i]]))
                raw.roots.push_back(append(i, append));

        if (!raw.Validate() || raw.num_joints() == 0)
            throw std::runtime_error("Invalid Ozz skeleton");

        rig result;
        result.skeleton = ozz::animation::offline::SkeletonBuilder{}(raw);
        if (!result.skeleton)
            throw std::runtime_error("Ozz skeleton build failed");

        for (auto joint : skin.joints)
        {
            auto it = std::find(ordered.begin(), ordered.end(), joint);
            if (it == ordered.end()) throw std::runtime_error("Skin remap missing joint");
            result.skin_joints.push_back(std::uint32_t(it - ordered.begin()));
        }

        for (const auto& source : asset.animations)
        {
            raw_clip clip;
            clip.name = source.name.c_str();
            clip.tracks.resize(ordered.size());
            float duration = 0.f;
            bool has_channels = false;

            for (const auto& channel : source.channels)
            {
                if (!channel.nodeIndex) continue;
                auto joint = std::find(ordered.begin(), ordered.end(), *channel.nodeIndex);
                if (joint == ordered.end()) continue;
                if (channel.path == fastgltf::AnimationPath::Weights) continue;

                const auto& sampler = source.samplers.at(channel.samplerIndex);
                if (sampler.interpolation != fastgltf::AnimationInterpolation::Linear)
                    throw std::runtime_error("Nonlinear glTF animation needs resampling");

                const auto& inputs = asset.accessors.at(sampler.inputAccessor);
                const auto& outputs = asset.accessors.at(sampler.outputAccessor);
                if (inputs.count != outputs.count)
                    throw std::runtime_error("Mismatched glTF animation channels");

                auto& track = clip.tracks[std::size_t(joint - ordered.begin())];
                for (std::size_t i = 0; i < inputs.count; ++i)
                {
                    const auto t = fastgltf::getAccessorElement<float>(asset, inputs, i);
                    duration = std::max(duration, t);
                    if (channel.path == fastgltf::AnimationPath::Translation)
                    {
                        const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                            asset, outputs, i);
                        track.translations.push_back({t, {p[0], p[1], p[2]}});
                    }
                    else if (channel.path == fastgltf::AnimationPath::Rotation)
                    {
                        const auto q = fastgltf::getAccessorElement<fastgltf::math::fvec4>(
                            asset, outputs, i);
                        track.rotations.push_back({t, {q[0], q[1], q[2], q[3]}});
                    }
                    else if (channel.path == fastgltf::AnimationPath::Scale)
                    {
                        const auto v = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                            asset, outputs, i);
                        track.scales.push_back({t, {v[0], v[1], v[2]}});
                    }
                }
                has_channels = true;
            }

            if (!has_channels) continue;
            clip.duration = std::max(duration, 0.001f);
            for (std::size_t i = 0; i < ordered.size(); ++i)
            {
                auto& track = clip.tracks[i];
                const auto pose = transform(asset.nodes[ordered[i]]);
                if (track.translations.empty())
                    track.translations.push_back({0, pose.translation});
                if (track.rotations.empty())
                    track.rotations.push_back({0, pose.rotation});
                if (track.scales.empty())
                    track.scales.push_back({0, pose.scale});
            }
            if (!clip.Validate())
                throw std::runtime_error("Invalid glTF animation after Ozz conversion");
            auto built = ozz::animation::offline::AnimationBuilder{}(clip);
            if (!built) throw std::runtime_error("Ozz animation build failed");
            result.names.emplace_back(source.name);
            result.clips.push_back(std::move(built));
        }
        return result;
    }

    ::scene::skin serialize_rig(const fastgltf::Asset& asset,
                                std::size_t skin_index, std::uint32_t id)
    {
        auto imported = import_rig(asset, skin_index);
        return serialize_rig(asset, skin_index, id, imported);
    }

    ::scene::skin serialize_rig(
        const fastgltf::Asset& asset, std::size_t skin_index, std::uint32_t id, const rig& imported
    )
    {
        ::scene::skin result;
        result.id = id;
        result.skeleton = archive(*imported.skeleton);
        result.inverse_bind.resize(imported.skeleton->num_joints());
        for (auto& m : result.inverse_bind)
            m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        const auto& source = asset.skins.at(skin_index);
        if (source.inverseBindMatrices)
        {
            const auto& accessor = asset.accessors.at(*source.inverseBindMatrices);
            if (accessor.count != imported.skin_joints.size())
                throw std::runtime_error("Mismatched glTF inverse bind matrices");
            for (std::size_t i = 0; i < accessor.count; ++i)
            {
                const auto matrix = fastgltf::getAccessorElement<fastgltf::math::fmat4x4>(
                    asset, accessor, i);
                auto& values = result.inverse_bind[imported.skin_joints[i]];
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        values[c * 4 + r] = matrix[c][r];
            }
        }
        for (std::size_t i = 0; i < imported.clips.size(); ++i)
            result.clips.push_back({imported.names[i], archive(*imported.clips[i])});
        return result;
    }
} // namespace srv::assets



// [srv/shared/net/animation.cpp]


#include <cmath>
#include <cstring>

namespace net::animation
{
    namespace
    {
        void write(std::uint8_t* out, std::uint32_t value)
        {
            out[0] = std::uint8_t(value >> 24);
            out[1] = std::uint8_t(value >> 16);
            out[2] = std::uint8_t(value >> 8);
            out[3] = std::uint8_t(value);
        }

        std::uint32_t read(const std::uint8_t* in)
        {
            return (std::uint32_t(in[0]) << 24) |
                (std::uint32_t(in[1]) << 16) |
                (std::uint32_t(in[2]) << 8) | std::uint32_t(in[3]);
        }
    }

    packet encode(const resource& state)
    {
        packet bytes{};
        bytes[0] = 'A';
        bytes[1] = 'N';
        bytes[2] = 'I';
        bytes[3] = 'M';
        write(bytes.data() + 4, 1);
        write(bytes.data() + 8, state.token);
        write(bytes.data() + 12, state.server_tick);
        write(bytes.data() + 16, state.instance_id);
        write(bytes.data() + 20, state.clip);
        write(bytes.data() + 24, state.start_tick);
        write(bytes.data() + 28, state.overlay);
        write(bytes.data() + 32, state.mask_root);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &state.blend, sizeof(bits));
        write(bytes.data() + 36, bits);
        return bytes;
    }

    std::optional<resource> decode(const std::uint8_t* bytes, std::size_t size)
    {
        if (!bytes || size != packet{}.size() ||
            bytes[0] != 'A' || bytes[1] != 'N' ||
            bytes[2] != 'I' || bytes[3] != 'M' ||
            read(bytes + 4) != 1)
            return std::nullopt;

        resource result;
        result.token = read(bytes + 8);
        result.server_tick = read(bytes + 12);
        result.instance_id = read(bytes + 16);
        result.clip = read(bytes + 20);
        result.start_tick = read(bytes + 24);
        result.overlay = read(bytes + 28);
        result.mask_root = read(bytes + 32);
        const auto bits = read(bytes + 36);
        std::memcpy(&result.blend, &bits, sizeof(result.blend));
        if (!result.token || !result.instance_id ||
            !std::isfinite(result.blend) ||
            result.blend < 0 || result.blend > 1)
            return std::nullopt;
        return result;
    }
    event_packet encode_event(const attachment_event& event)
    {
        event_packet bytes{};
        bytes[0] = 'E';
        bytes[1] = 'V';
        bytes[2] = 'N';
        bytes[3] = 'T';
        write(bytes.data() + 4, event.token);
        write(bytes.data() + 8, event.tick);
        write(bytes.data() + 12, event.sequence);
        write(bytes.data() + 16, event.instance_id);
        write(bytes.data() + 20, event.joint);
        write(bytes.data() + 24, event.kind);
        return bytes;
    }

    std::optional<attachment_event> decode_event(const std::uint8_t* bytes,
                                                  std::size_t size)
    {
        if (!bytes || size != event_packet{}.size() ||
            bytes[0] != 'E' || bytes[1] != 'V' ||
            bytes[2] != 'N' || bytes[3] != 'T')
            return std::nullopt;
        attachment_event event;
        event.token = read(bytes + 4);
        event.tick = read(bytes + 8);
        event.sequence = read(bytes + 12);
        event.instance_id = read(bytes + 16);
        event.joint = read(bytes + 20);
        event.kind = read(bytes + 24);
        return event.token && event.instance_id && event.sequence ?
            std::optional<attachment_event>(event) : std::nullopt;
    }
} // namespace net::animation



// [srv/shared/net/message.cpp]


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
            data[3] != 'M' || data[4] != 1 || data[5] < 1 || data[5] > 6 || data[6] || data[7])
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



// [srv/shared/net/scene_stream.cpp]
// Data-oriented scene codec: fixed-field packets, contiguous payloads, bounded input.
#include <stb_image.h>
#include <bit>
#include <climits>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace net::scene_stream {
namespace {
    using bytes=std::vector<std::uint8_t>;
    using vec3=vk::util::math::vec3;

    struct cursor {const bytes& data;std::size_t pos;};
    std::size_t left(const cursor& c) {return c.data.size()-c.pos;}

    void put(bytes& b,std::uint32_t value) {
        b.push_back(std::uint8_t(value>>24));
        b.push_back(std::uint8_t(value>>16));
        b.push_back(std::uint8_t(value>>8));
        b.push_back(std::uint8_t(value));
    }
    void put(bytes& b,float value) {
        if(!std::isfinite(value))throw std::runtime_error("Non-finite scene value");
        put(b,std::bit_cast<std::uint32_t>(value));
    }
    void put(bytes& b,vec3 value) {
        put(b,value.x);put(b,value.y);put(b,value.z);
    }
    template<class... T>
    void put_all(bytes& b,const T&... values) {(put(b,values),...);}

    bool take(cursor& c,std::uint32_t& value) {
        if(left(c)<4)return false;
        const auto* p=c.data.data()+c.pos;
        value=(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|
              (std::uint32_t(p[2])<<8)|std::uint32_t(p[3]);
        c.pos+=4;
        return true;
    }
    bool take(cursor& c,float& value) {
        std::uint32_t bits=0;
        if(!take(c,bits))return false;
        value=std::bit_cast<float>(bits);
        return std::isfinite(value);
    }
    bool take(cursor& c,vec3& value) {
        return take(c,value.x)&&take(c,value.y)&&take(c,value.z);
    }
    template<class... T>
    bool take_all(cursor& c,T&... values) {return (take(c,values)&&...);}

    void material_put(bytes& out,const ::scene::material& m) {
        for(auto x:m.base_color)put(out,x);
        for(auto x:m.emissive)put(out,x);
        put_all(out,m.metallic,m.roughness,m.occlusion,m.alpha_cutoff,
            m.base_color_texture,m.metallic_roughness_texture,
            m.normal_texture,m.occlusion_texture,m.emissive_texture,
            m.alpha_mode,std::uint32_t(m.double_sided),m.procedural_pattern);
    }
    bool material_take(cursor& in,::scene::material& m) {
        for(auto& x:m.base_color)if(!take(in,x))return false;
        for(auto& x:m.emissive)if(!take(in,x))return false;
        std::uint32_t double_sided=0;
        if(!take_all(in,m.metallic,m.roughness,m.occlusion,m.alpha_cutoff,
            m.base_color_texture,m.metallic_roughness_texture,m.normal_texture,
            m.occlusion_texture,m.emissive_texture,m.alpha_mode,
            double_sided,m.procedural_pattern))return false;
        if(double_sided>1||m.alpha_mode>2||m.procedural_pattern>1)return false;
        m.double_sided=double_sided!=0;
        return true;
    }
    void append(bytes& out,const bytes& src) {
        out.insert(out.end(),src.begin(),src.end());
    }
    std::uint32_t hash(const std::uint8_t* src,std::size_t size) {
        std::uint32_t result=2166136261u;
        for(std::size_t i=0;i<size;++i)result=(result^src[i])*16777619u;
        return result;
    }
    bool valid_box(const vec3& a,const vec3& b) {
        return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z)&&
               std::isfinite(b.x)&&std::isfinite(b.y)&&std::isfinite(b.z)&&
               a.x<b.x&&a.y<b.y&&a.z<b.z;
    }
    bool valid_light(const ::scene::point_light& l) {
        return std::isfinite(l.radius)&&l.radius>0&&
               std::isfinite(l.strength)&&l.strength>=0&&
               l.color.x>=0&&l.color.y>=0&&l.color.z>=0;
    }
}

std::vector<std::uint8_t> serialize(const ::scene::state& scene) {
    if(scene.meshes.size()>max_scene_bytes/12 ||
       scene.instances.size()>max_scene_bytes/48 ||scene.lights.size()>64)
        throw std::runtime_error("Scene snapshot exceeds maximum batch size");
    bytes out{'S','C','N','A'};
    put_all(out,std::uint32_t(scene.meshes.size()),
        std::uint32_t(scene.instances.size()),std::uint32_t(scene.textures.size()),
        std::uint32_t(scene.skins.size()),
        std::uint32_t(scene.collision_boxes.size()),
        std::uint32_t(scene.lights.size()));

    for(const auto& t:scene.textures) {
        const bool encoded=!t.encoded.empty();
        const auto& payload=encoded?t.encoded:t.pixels;
        const auto raw=std::uint64_t(t.width)*t.height*4u;
        if(!t.id||!t.width||!t.height||raw>max_scene_bytes-20||
           (!encoded&&raw!=t.pixels.size())||payload.empty()||
           payload.size()>max_scene_bytes-20||
           out.size()+20+payload.size()>max_scene_bytes)
            throw std::runtime_error("Invalid texture or scene batch too large");
        put_all(out,t.id,t.width,t.height,std::uint32_t(encoded),
            std::uint32_t(payload.size()));
        append(out,payload);
    }

    for(const auto& skin:scene.skins) {
        if(!skin.id||skin.skeleton.empty()||skin.inverse_bind.empty()||
           skin.inverse_bind.size()>1024)
            throw std::runtime_error("Invalid skeletal asset");
        put_all(out,skin.id,std::uint32_t(skin.inverse_bind.size()),
            std::uint32_t(skin.skeleton.size()),std::uint32_t(skin.clips.size()));
        append(out,skin.skeleton);
        for(const auto& matrix:skin.inverse_bind)
            for(float value:matrix)put(out,value);
        for(const auto& clip:skin.clips) {
            if(clip.name.size()>255||clip.bytes.empty())
                throw std::runtime_error("Invalid skeletal animation clip");
            put(out,std::uint32_t(clip.name.size()));
            out.insert(out.end(),clip.name.begin(),clip.name.end());
            put(out,std::uint32_t(clip.bytes.size()));
            append(out,clip.bytes);
        }
        if(out.size()>max_scene_bytes)
            throw std::runtime_error("Skeletal asset exceeds scene batch budget");
    }

    for(const auto& m:scene.meshes) {
        if(m.vertices.empty()||m.indices.empty()||m.indices.size()%3||
           m.vertices.size()>max_scene_bytes/64||m.indices.size()>max_scene_bytes/4)
            throw std::runtime_error("Invalid scene mesh");
        put_all(out,m.id,m.skin_id,std::uint32_t(m.vertices.size()),
            std::uint32_t(m.indices.size()));
        material_put(out,m.surface);
        for(const auto& v:m.vertices) {
            put_all(out,v.position,v.normal,v.u,v.v);
            if(m.skin_id) {
                for(auto joint:v.joints)put(out,joint);
                for(auto weight:v.weights)put(out,weight);
            }
        }
        for(auto index:m.indices) {
            if(index>=m.vertices.size())
                throw std::runtime_error("Scene mesh index out of bounds");
            put(out,index);
        }
        if(out.size()>max_scene_bytes)
            throw std::runtime_error("Scene snapshot exceeds maximum batch size");
    }

    for(const auto& i:scene.instances) {
        put_all(out,i.id,i.mesh_id,i.animation,i.animation_tick,
            i.overlay_animation,i.overlay_root,i.overlay_weight,
            i.pose.position,i.pose.scale,i.pose.rotation.x,
            i.pose.rotation.y,i.pose.rotation.z,i.pose.rotation.w);
    }
    for(const auto& b:scene.collision_boxes) {
        if(!valid_box(b.minimum,b.maximum))
            throw std::runtime_error("Invalid scene collision bounds");
        put_all(out,b.minimum,b.maximum);
    }
    for(const auto& l:scene.lights) {
        if(!valid_light(l))throw std::runtime_error("Invalid scene light");
        put_all(out,l.position,l.radius,l.color,l.strength);
    }
    if(out.size()>max_scene_bytes)
        throw std::runtime_error("Scene snapshot exceeds maximum batch size");
    return out;
}

bool deserialize(const std::vector<std::uint8_t>& bytes,::scene::state& scene) {
    if(bytes.size()<28||bytes.size()>max_scene_bytes||
       bytes[0]!='S'||bytes[1]!='C'||bytes[2]!='N'||bytes[3]!='A')
        return false;
    cursor in{bytes,4};
    std::uint32_t meshes=0,instances=0,textures=0,skins=0,boxes=0,lights=0;
    if(!take_all(in,meshes,instances,textures,skins,boxes,lights)||
       lights>64||lights>left(in)/32||boxes>left(in)/24||
       textures>left(in)/20||skins>left(in)/16||
       meshes>left(in)/12||instances>left(in)/48)return false;

    ::scene::state result;
    result.meshes.reserve(meshes);
    result.instances.reserve(instances);
    result.textures.reserve(textures);
    result.skins.reserve(skins);
    result.collision_boxes.reserve(boxes);
    result.lights.reserve(lights);

    for(std::uint32_t i=0;i<textures;++i) {
        ::scene::texture t;
        std::uint32_t encoding=0,length=0;
        if(!take_all(in,t.id,t.width,t.height,encoding,length)||
           !t.id||!t.width||!t.height||encoding>1||length>left(in)||
           std::uint64_t(t.width)*t.height*4u>max_scene_bytes-20)return false;
        if(encoding==0) {
            if(std::uint64_t(t.width)*t.height*4u!=length)return false;
            t.pixels.assign(bytes.begin()+in.pos,bytes.begin()+in.pos+length);
        } else {
            if(length>INT_MAX||t.width>INT_MAX||t.height>INT_MAX)return false;
            int w=0,h=0,c=0;
            auto* p=stbi_load_from_memory(bytes.data()+in.pos,int(length),&w,&h,&c,4);
            if(!p||w!=int(t.width)||h!=int(t.height)) {
                stbi_image_free(p);
                return false;
            }
            t.pixels.assign(p,p+std::size_t(w)*h*4u);
            stbi_image_free(p);
        }
        in.pos+=length;
        result.textures.push_back(std::move(t));
    }

    for(std::uint32_t i=0;i<skins;++i) {
        ::scene::skin skin;
        std::uint32_t joints=0,length=0,clips=0;
        if(!take_all(in,skin.id,joints,length,clips)||
           !skin.id||!joints||joints>1024||length>left(in)||
           std::uint64_t(joints)*64>left(in)-length||
           clips>max_scene_bytes/8)return false;
        skin.skeleton.assign(bytes.begin()+in.pos,bytes.begin()+in.pos+length);
        in.pos+=length;
        skin.inverse_bind.resize(joints);
        for(auto& matrix:skin.inverse_bind)
            for(auto& value:matrix)if(!take(in,value))return false;
        for(std::uint32_t j=0;j<clips;++j) {
            std::uint32_t name=0,size=0;
            if(!take(in,name)||name>255||name>left(in))return false;
            ::scene::clip clip;
            clip.name.assign(reinterpret_cast<const char*>(bytes.data()+in.pos),name);
            in.pos+=name;
            if(!take(in,size)||!size||size>left(in))return false;
            clip.bytes.assign(bytes.begin()+in.pos,bytes.begin()+in.pos+size);
            in.pos+=size;
            skin.clips.push_back(std::move(clip));
        }
        result.skins.push_back(std::move(skin));
    }

    for(std::uint32_t i=0;i<meshes;++i) {
        ::scene::mesh mesh;
        std::uint32_t vertices=0,indices=0;
        if(!take_all(in,mesh.id,mesh.skin_id,vertices,indices)||
           !vertices||!indices||indices%3||left(in)<76||
           std::uint64_t(vertices)*(mesh.skin_id?64u:32u)+
           std::uint64_t(indices)*4u>left(in)-76)return false;
        if(!material_take(in,mesh.surface))return false;
        mesh.vertices.resize(vertices);
        mesh.indices.resize(indices);
        for(auto& v:mesh.vertices) {
            if(!take_all(in,v.position,v.normal,v.u,v.v))return false;
            if(mesh.skin_id) {
                for(auto& joint:v.joints)if(!take(in,joint))return false;
                for(auto& weight:v.weights)
                    if(!take(in,weight)||weight<0||weight>1)return false;
            }
        }
        for(auto& index:mesh.indices)
            if(!take(in,index)||index>=vertices)return false;
        result.meshes.push_back(std::move(mesh));
    }

    if(instances>left(in)/48)return false;
    for(std::uint32_t i=0;i<instances;++i) {
        ::scene::instance item;
        if(!take_all(in,item.id,item.mesh_id,item.animation,item.animation_tick,
           item.overlay_animation,item.overlay_root,item.overlay_weight,
           item.pose.position,item.pose.scale,item.pose.rotation.x,
           item.pose.rotation.y,item.pose.rotation.z,item.pose.rotation.w)||
           item.overlay_weight<0||item.overlay_weight>1)return false;
        result.instances.push_back(std::move(item));
    }
    for(std::uint32_t i=0;i<boxes;++i) {
        ::scene::collision_box b{};
        if(!take_all(in,b.minimum,b.maximum)||!valid_box(b.minimum,b.maximum))
            return false;
        result.collision_boxes.push_back(b);
    }
    for(std::uint32_t i=0;i<lights;++i) {
        ::scene::point_light l{};
        if(!take_all(in,l.position,l.radius,l.color,l.strength)||!valid_light(l))
            return false;
        result.lights.push_back(l);
    }
    if(left(in))return false;
    scene=std::move(result);
    return true;
}

std::uint32_t checksum(const std::vector<std::uint8_t>& bytes) {
    return hash(bytes.data(),bytes.size());
}
std::vector<std::uint8_t> encode_batch(const std::vector<std::uint8_t>& bytes,
                                      std::uint32_t token,std::uint32_t batch,bool final) {
    if(bytes.empty()||bytes.size()>max_scene_bytes||!token||batch>=65535)
        throw std::invalid_argument("invalid scene batch");
    std::vector<std::uint8_t> result{'V','G','S','C',1,std::uint8_t(final),0,0};
    put_all(result,token,batch,checksum(bytes));
    append(result,bytes);
    return result;
}
bool decode_batch(const std::vector<std::uint8_t>& bytes,batch_payload& result) {
    if(bytes.size()<=20||bytes.size()>max_scene_bytes+20||
       bytes[0]!='V'||bytes[1]!='G'||bytes[2]!='S'||bytes[3]!='C'||
       bytes[4]!=1||bytes[5]>1||bytes[6]||bytes[7])return false;
    cursor in{bytes,8};
    std::uint32_t token=0,batch=0,expected=0;
    if(!take_all(in,token,batch,expected)||!token||batch>=65535)return false;
    const auto got=hash(bytes.data()+20,bytes.size()-20);
    if(expected!=got)return false;
    result={token,batch,bytes[5]!=0,
        std::vector<std::uint8_t>(bytes.begin()+20,bytes.end())};
    return true;
}
} // namespace net::scene_stream

// [srv/shared/net/weapon.cpp]




namespace net::weapon
{
    namespace
    {
        using namespace net::movement::detail;

        bool
        header(const std::uint8_t* p, std::size_t size, std::size_t expected, std::uint8_t kind)
        {
            return p && size == expected && p[0] == 'V' && p[1] == 'G' && p[2] == 'U' &&
                   p[3] == 'N' && p[4] == 2 && p[5] == kind && !p[6] && !p[7];
        }

        void write_state(std::uint8_t* p, const snapshot& v)
        {
            put(p + 8, v.token);
            put(p + 12, v.sequence);
            put(p + 16, v.tick);
            put(p + 20, v.magazine);
            put(p + 24, v.reserve);
            put(p + 28, std::uint32_t(v.reload));
            put(p + 32, std::uint32_t(v.outcome));
            put_float(p + 36, v.reload_elapsed);
            put_float(p + 40, v.reload_duration);
            put_float(p + 44, v.refill_time);
            put(p + 48, v.weapon_id);
            put(p + 52, v.active_weapon_id);
        }

        std::optional<snapshot> read_state(const std::uint8_t* p)
        {
            snapshot v;
            v.token = get(p + 8);
            v.sequence = get(p + 12);
            v.tick = get(p + 16);
            v.magazine = get(p + 20);
            v.reserve = get(p + 24);
            v.reload = reload_phase(get(p + 28));
            v.outcome = result(get(p + 32));
            v.reload_elapsed = get_float(p + 36);
            v.reload_duration = get_float(p + 40);
            v.refill_time = get_float(p + 44);
            v.weapon_id = get(p + 48);
            v.active_weapon_id = get(p + 52);
            if (!v.token || !v.weapon_id || !v.active_weapon_id || get(p + 28) > 2 ||
                get(p + 32) > std::uint32_t(result::equipped) ||
                !std::isfinite(v.reload_elapsed) || !std::isfinite(v.reload_duration) ||
                !std::isfinite(v.refill_time) || v.reload_elapsed < 0 || v.reload_duration < 0 ||
                v.refill_time < 0 ||
                v.reload_elapsed > v.reload_duration || v.refill_time > v.reload_duration)
            {
                return std::nullopt;
            }
            return v;
        }
    } // namespace

    command_packet encode_command(const command& v)
    {
        command_packet p{'V', 'G', 'U', 'N', 2, 1, 0, 0};
        put(p.data() + 8, v.token);
        put(p.data() + 12, v.sequence);
        put(p.data() + 16, std::uint32_t(v.kind));
        put_float(p.data() + 20, v.yaw);
        put_float(p.data() + 24, v.pitch);
        put(p.data() + 28, v.weapon_id);
        return p;
    }

    std::optional<command> decode_command(const std::uint8_t* p, std::size_t size)
    {
        if (!header(p, size, command_packet{}.size(), 1))
        {
            return std::nullopt;
        }
        command v{
            get(p + 8), get(p + 12), action(get(p + 16)), get_float(p + 20), get_float(p + 24), get(p + 28)
        };
        if (!v.token || !v.sequence || !v.weapon_id || get(p + 16) < 1 || get(p + 16) > 3 ||
            !std::isfinite(v.yaw) || !std::isfinite(v.pitch) || std::abs(v.yaw) > 1000 ||
            std::abs(v.pitch) > 1.570797f)
        {
            return std::nullopt;
        }
        return v;
    }

    snapshot_packet encode_snapshot(const snapshot& v)
    {
        snapshot_packet p{'V', 'G', 'U', 'N', 2, 2, 0, 0};
        write_state(p.data(), v);
        return p;
    }

    std::optional<snapshot> decode_snapshot(const std::uint8_t* p, std::size_t size)
    {
        if (!header(p, size, snapshot_packet{}.size(), 2))
        {
            return std::nullopt;
        }
        return read_state(p);
    }

    shot_packet encode_shot(const shot& v)
    {
        shot_packet p{'V', 'G', 'U', 'N', 2, 3, 0, 0};
        write_state(p.data(), v.state);
        const auto& h = v.impact;
        put(p.data() + 56, h.impacted ? 1 : 0);
        put_float(p.data() + 60, h.position.x);
        put_float(p.data() + 64, h.position.y);
        put_float(p.data() + 68, h.position.z);
        put_float(p.data() + 72, h.normal.x);
        put_float(p.data() + 76, h.normal.y);
        put_float(p.data() + 80, h.normal.z);
        put(p.data() + 84, h.instance);
        put(p.data() + 88, h.triangle);
        return p;
    }

    std::optional<shot> decode_shot(const std::uint8_t* p, std::size_t size)
    {
        if (!header(p, size, shot_packet{}.size(), 3))
        {
            return std::nullopt;
        }
        const auto state = read_state(p);
        if (!state || get(p + 56) > 1)
        {
            return std::nullopt;
        }
        shot v{
            *state,
            {get(p + 56) == 1,
             {get_float(p + 60), get_float(p + 64), get_float(p + 68)},
             {get_float(p + 72), get_float(p + 76), get_float(p + 80)},
             get(p + 84),
             get(p + 88)}
        };
        const auto& h = v.impact;
        auto finite = [](float x)
        {
            return std::isfinite(x) && std::abs(x) < 1000000;
        };
        if (!finite(h.position.x) || !finite(h.position.y) || !finite(h.position.z) ||
            !finite(h.normal.x) || !finite(h.normal.y) || !finite(h.normal.z))
        {
            return std::nullopt;
        }
        if (h.impacted && (state->outcome != result::fired || !h.instance ||
                           std::abs(vk::util::math::length_squared(h.normal) - 1.0f) > 0.01f))
        {
            return std::nullopt;
        }
        return v;
    }
} // namespace net::weapon



// [srv/shared/player/movement.cpp]
// Fixed-tick data update: one flat player state, contiguous colliders and spatial index.
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <numeric>
namespace player {
namespace {
    using namespace vk::util::math;
    std::optional<parameters_t> configured;

    bool overlaps_xz(vec3 p,const box& b) {
        const float r=parameters().radius;
        return p.x+r>b.min.x && p.x-r<b.max.x &&
               p.z+r>b.min.z && p.z-r<b.max.z;
    }
    bool overlaps_y(float y,const box& b) {
        return y<b.max.y && y+parameters().height>b.min.y;
    }
}
void configure(parameters_t p) {
    if(!std::isfinite(p.spawn.x)||!std::isfinite(p.spawn.y)||
       !std::isfinite(p.spawn.z)||!std::isfinite(p.walk_speed)||
       !std::isfinite(p.sprint_speed)||!std::isfinite(p.gravity)||
       !std::isfinite(p.radius)||!std::isfinite(p.height)||
       p.walk_speed<=0||p.sprint_speed<=0||p.gravity<=0||
       p.radius<=0||p.height<=0)
        throw std::invalid_argument("Player parameters must be specified by the server");
    configured=p;
}
const parameters_t& parameters() {
    if(!configured)throw std::logic_error("Player parameters not yet received from server");
    return *configured;
}
collision_world build_colliders(const ::scene::state& scene) {
    return build_colliders(scene.meshes,scene.instances);
}
collision_world build_colliders(const std::vector<::scene::mesh>& meshes,
                                const std::vector<::scene::instance>& instances) {
    using namespace vk::util::math;
    collision_world boxes;
    boxes.reserve(instances.size());
    for(const auto& instance:instances) {
        const auto mesh=std::find_if(meshes.begin(),meshes.end(),
            [&](const ::scene::mesh& m){return m.id==instance.mesh_id;});
        if(mesh==meshes.end()||mesh->vertices.empty())continue;
        const float limit=std::numeric_limits<float>::max();
        box b{{limit,limit,limit},{-limit,-limit,-limit}};
        const auto& pose=instance.pose;
        for(const auto& v:mesh->vertices) {
            const vec3 scaled{
                v.position.x*pose.scale.x,v.position.y*pose.scale.y,
                v.position.z*pose.scale.z};
            const vec3 p=pose.position+rotate(pose.rotation,scaled);
            b.min={std::min(b.min.x,p.x),std::min(b.min.y,p.y),std::min(b.min.z,p.z)};
            b.max={std::max(b.max.x,p.x),std::max(b.max.y,p.y),std::max(b.max.z,p.z)};
        }
        boxes.push_back(b);
    }
    return boxes;
}
spatial::index build_index(const collision_world& boxes) {
    std::vector<spatial::bounds> bounds;
    bounds.reserve(boxes.size());
    for(const auto& box:boxes)bounds.push_back({box.min,box.max});
    spatial::index index;
    index.rebuild(std::move(bounds));
    return index;
}
void step(state& p,const input& keys,const collision_world& world,
          const spatial::index* broadphase) {
    using namespace vk::util::math;
    const auto& settings=parameters();
    const float radius=settings.radius,height=settings.height;
    const float dt=1.f/float(net::limits::tick_hz);
    const vec3 front{std::sin(keys.yaw),0,-std::cos(keys.yaw)};
    const vec3 side{std::cos(keys.yaw),0,std::sin(keys.yaw)};
    vec3 movement{};
    if(keys.buttons&forward)movement=movement+front;
    if(keys.buttons&backward)movement=movement-front;
    if(keys.buttons&right)movement=movement+side;
    if(keys.buttons&left)movement=movement-side;
    if(length_squared(movement)>1.f)movement=normalize(movement);
    const float speed=keys.buttons&sprint?settings.sprint_speed:settings.walk_speed;
    p.velocity.x=movement.x*speed;
    p.velocity.z=movement.z*speed;
    const vec3 previous=p.position;

    // Reuse candidate storage across fixed ticks; no per-frame heap churn.
    thread_local std::vector<std::uint32_t> candidates;
    candidates.clear();
    if(broadphase) {
        const float horizontal=radius+std::max(settings.walk_speed,settings.sprint_speed)*dt+.01f;
        const float vertical=std::abs(p.velocity.y)*dt+settings.gravity*dt*dt+.05f;
        broadphase->query({{
            p.position.x-horizontal,p.position.y-vertical,p.position.z-horizontal},{
            p.position.x+horizontal,p.position.y+height+vertical,p.position.z+horizontal}},
            [&](std::uint32_t id){candidates.push_back(id);});
    } else {
        candidates.resize(world.size());
        std::iota(candidates.begin(),candidates.end(),0u);
    }

    // Resolve X and Z with the same axis-separated rule.
    for(int axis=0;axis<2;++axis) {
        float& position=axis?p.position.z:p.position.x;
        const float velocity=axis?p.velocity.z:p.velocity.x;
        const float old=position;
        position+=velocity*dt;
        for(const auto id:candidates) {
            const auto& b=world[id];
            if(!overlaps_y(p.position.y,b)||!overlaps_xz(p.position,b))continue;
            const float lo=axis?b.min.z:b.min.x;
            const float hi=axis?b.max.z:b.max.x;
            if(velocity>0&&old+radius<=lo)position=lo-radius;
            else if(velocity<0&&old-radius>=hi)position=hi+radius;
        }
    }

    p.velocity.y-=settings.gravity*dt;
    p.position.y+=p.velocity.y*dt;
    p.grounded=false;
    for(const auto id:candidates) {
        const auto& b=world[id];
        if(!overlaps_xz(p.position,b))continue;
        if(p.velocity.y<=0 && previous.y>=b.max.y-.0001f &&
           p.position.y<b.max.y) {
            p.position.y=b.max.y;
            p.velocity.y=0;
            p.grounded=true;
        } else if(p.velocity.y>0 && previous.y+height<=b.min.y &&
                  p.position.y+height>b.min.y) {
            p.position.y=b.min.y-height;
            p.velocity.y=0;
        }
    }
}
} // namespace player

// [srv/game/options.cpp]


#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace game::options
{
    state* get()
    {
        static state data;
        return &data;
    }

    void init(int argc, char** argv, mode selected_mode, state configured)
    {
        state result = std::move(configured);
        result.mode = selected_mode;
        bool host_specified = false;
        bool bind_specified = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string option = argv[i];

            if (option == "--help")
            {
                result.help = true;
                continue;
            }

            if (option == "--headless")
            {
                result.headless = true;
                continue;
            }

            if (option != "--port" && option != "--duration" && option != "--host" &&
                option != "--bind")
            {
                throw std::invalid_argument("Unknown option: " + option);
            }

            if (++i == argc)
            {
                throw std::invalid_argument("Missing value for " + option);
            }

            const std::string value = argv[i];
            std::size_t end = 0;

            if (option == "--port")
            {
                const auto port = std::stoul(value, &end);

                if (end != value.size() || port == 0 || port > 65535 || value.front() == '-')
                {
                    throw std::invalid_argument("Port must be in 1..65535");
                }

                result.port = static_cast<std::uint16_t>(port);
            }
            else if (option == "--duration")
            {
                result.duration = std::stod(value, &end);

                if (end != value.size() || !std::isfinite(result.duration) || result.duration <= 0)
                {
                    throw std::invalid_argument("Duration must be a positive number of seconds");
                }
            }
            else if (option == "--host")
            {
                result.host = value;
                host_specified = true;
            }
            else
            {
                result.bind_host = value;
                bind_specified = true;
            }
        }

        if (!result.help)
        {
            if (result.mode == mode::server_only && result.headless)
            {
                throw std::invalid_argument("--headless requires a client mode");
            }

            if (result.mode != mode::client_only && host_specified)
            {
                throw std::invalid_argument(
                    "--host is only supported by vulkan-client"
                );
            }

            if (result.mode == mode::client_only && bind_specified)
            {
                throw std::invalid_argument("--bind is only supported by vulkan-server");
            }
        }

        if (!result.port || (result.mode == mode::server_only && result.bind_host.empty()))
            throw std::invalid_argument("A server bind address and UDP port are required");

        if (result.mode == mode::client_only)
        {
            const auto& g = result.graphics;
            if (result.host.empty() || g.width <= 0 || g.height <= 0 ||
                g.title.empty() || g.vertex_shader.empty() || g.fragment_shader.empty() ||
                g.gbuffer == gbuffer_precision::unspecified ||
                g.surface == surface_format::unspecified ||
                g.depth == depth_precision::unspecified ||
                g.present == presentation::unspecified ||
                g.culling_shader.empty() || g.lighting_shader.empty() ||
                g.tonemap_shader.empty() || g.fullscreen_shader.empty() ||
                g.presentation_shader.empty() || g.exposure < 0)
                throw std::invalid_argument("Client graphics configuration is incomplete");
        }
        *get() = std::move(result);
    }

    void print_help()
    {
        const bool server = get()->mode == mode::server_only;
        std::cout << (server ? "Usage: vulkan-server [OPTIONS]\n\n"
                            : "Usage: vulkan-client [OPTIONS]\n\n")
                  << "Options:\n"
                  << "  --port PORT         UDP port (configured in server.cpp or client.cpp)\n";
        if (server)
            std::cout << "  --bind ADDRESS      Server listen address (server.cpp)\n";
        else
            std::cout << "  --host HOST         Server address (client.cpp)\n"
                      << "  --headless          Run the client without a window\n";
        std::cout << "  --duration SECONDS  Stop automatically after the given time\n"
                  << "  --help              Show this help\n";
    }

    void shutdown()
    {
        *get() = {};
    }
} // namespace game::options



// [srv/game/state.cpp]


namespace game::application
{
    state* get()
    {
        static state data;
        return &data;
    }
} // namespace game::application

