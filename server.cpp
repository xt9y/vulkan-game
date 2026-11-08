// Amalgamated original full-feature Vulkan game server.
// No references to game-local headers or compilation of clt/srv files.
// Preserves original fastgltf and Ozz asset/animation paths.

// [server.cpp]

// [srv/app/application.hpp]


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


#include <network/secure.hpp>
#include <network/endpoint.hpp>
#include <network/session.hpp>

// [srv/shared/net/weapon.hpp]


// [srv/shared/weapon/weapon.hpp]



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





#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace srv::application
{
    struct model_request
    {
        std::filesystem::path path;
        ::scene::transform pose;
        std::uint32_t animation;
    };

    struct weapon_command_context
    {
        const net::weapon::command& command;
        const ::weapon::definition& definition;
        ::weapon::state& state;
        const ::player::state& player;
        double now;
        const ::weapon::world& world;
    };

    using weapon_command_handler = std::function<::weapon::hit(weapon_command_context&)>;

    struct state
    {
        game::options::state options;
        net::secure::resource socket;
        std::unique_ptr<net::session> scene_transport;
        scene::state scene;
        // Live authoritative point-light versions for editor replication.
        std::vector<std::uint32_t> editor_light_versions;
        std::uint32_t editor_light_revision=1;
        std::vector<model_request> models;
        bool scene_configured = false;
        std::shared_ptr<scene::state> importing;
        std::size_t next_texture = 0;
        std::size_t next_skin = 0;
        std::size_t next_mesh = 0;
        std::size_t next_instance = 0;
        std::size_t animation_cursor = 0;
        struct pending_event { net::animation::attachment_event event; std::uint32_t repeats = 3; };
        std::vector<pending_event> attachment_events;
        std::uint32_t next_event = 0;
        player::collision_world colliders;
        ::weapon::world weapon_world;
        ::weapon::registry weapons;
        weapon_command_handler on_weapon_command;
        std::function<bool(const net::endpoint::resource&)> editor_permission;
        spatial::index collision_index;
        struct batch {
            std::vector<std::uint8_t> bytes;
            bool final=false;
        };
        std::vector<batch> scene_batches;
        std::uint32_t tick = 0;
        bool initialized = false;
    };

    state* get();
    // The game defines both its initial scene and model imports before init().
    void set_scene(::scene::state scene);
    void load_model(std::filesystem::path path, ::scene::transform pose,
                    std::uint32_t animation);
    // Register game rules and receive authenticated, owned, equipped weapon commands.
    void add_weapon(::weapon::definition definition);
    void on_weapon_command(weapon_command_handler handler);
    // No client can change the level unless this policy explicitly permits it.
    void on_editor_permission(std::function<bool(const net::endpoint::resource&)> handler);
    void init();
    // Publish append-only mesh/instance batches; called on the server thread.
    // Background jobs return a completion that invokes append_scene().
    void begin_stream();
    void append_scene(::scene::state patch, bool final = false);
    // Call on the authoritative server tick (not from an import worker).
    bool play_animation(std::uint32_t instance_id, std::uint32_t clip,
                        std::uint32_t overlay = UINT32_MAX, float blend = 0.0f,
                        std::uint32_t mask_root = UINT32_MAX);
    // Send a named game effect at a skeleton socket; replicated to clients.
    bool emit_attachment_event(std::uint32_t instance_id, std::uint32_t joint,
                               std::uint32_t kind);
    int run();
    void shutdown() noexcept;
} // namespace srv::application



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



// [weapons.hpp]


// [srv/shared/weapon/gun.hpp]



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


#include <vector>

// Game-owned gun catalog. Both server and client consume the same complete
// profiles: the server reads authoritative rules, the client loads models,
// animation clips, controls, ADS behavior, recoil and effects.
namespace game_weapons
{
    std::vector<::weapon::gun> all();
}



#include <algorithm>
#include <array>
#include <cstdint>
#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
    void quad(::scene::mesh& mesh, vk::util::math::vec3 a, vk::util::math::vec3 b, vk::util::math::vec3 c, vk::util::math::vec3 d, vk::util::math::vec3 normal)
    {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());

        mesh.vertices.push_back({a, normal, 0, 0});
        mesh.vertices.push_back({b, normal, 1, 0});
        mesh.vertices.push_back({c, normal, 1, 1});
        mesh.vertices.push_back({d, normal, 0, 1});

        mesh.indices.insert(mesh.indices.end(), {
            base, base + 1, base + 2,
            base, base + 2, base + 3,
        });
    }

    ::scene::mesh make_rect(std::uint32_t id, float x, float y, float z,
                            std::array<float, 4> color, float roughness = 0.85f)
    {
        ::scene::mesh mesh;
        mesh.id = id;
        mesh.surface.base_color = color;
        mesh.surface.roughness = roughness;
    
        quad(mesh, {-x, -y,  z}, { x, -y,  z}, { x,  y,  z}, {-x,  y,  z}, { 0,  0,  1});
        quad(mesh, { x, -y, -z}, {-x, -y, -z}, {-x,  y, -z}, { x,  y, -z}, { 0,  0, -1});
        quad(mesh, {-x, -y, -z}, {-x, -y,  z}, {-x,  y,  z}, {-x,  y, -z}, {-1,  0,  0});
        quad(mesh, { x, -y,  z}, { x, -y, -z}, { x,  y, -z}, { x,  y,  z}, { 1,  0,  0});
        quad(mesh, {-x,  y,  z}, { x,  y,  z}, { x,  y, -z}, {-x,  y, -z}, { 0,  1,  0});
        quad(mesh, {-x, -y, -z}, { x, -y, -z}, { x, -y,  z}, {-x, -y,  z}, { 0, -1,  0});
    
        return mesh;
    }

    void place(::scene::state& initial_scene,std::uint32_t id,
               std::uint32_t mesh,vk::util::math::vec3 position)
    {
        ::scene::instance object{};
        object.id = id;
        object.mesh_id = mesh;
        object.pose.position = position;

        // World collision bounds follow actual transformed scene vertices.
        const auto geometry = std::find_if(
            initial_scene.meshes.begin(), initial_scene.meshes.end(),
            [mesh](const ::scene::mesh& item) { return item.id == mesh; });
        if (geometry == initial_scene.meshes.end())
            throw std::runtime_error("No mesh for scene collision bounds");

        ::scene::collision_box bounds{};
        const auto infinity = std::numeric_limits<float>::infinity();
        bounds.minimum = { infinity, infinity, infinity };
        bounds.maximum = {-infinity,-infinity,-infinity };
        for (const auto& vertex : geometry->vertices)
        {
            const auto& p = vertex.position;
            const auto& scale = object.pose.scale;
            const auto local = vk::util::math::vec3{
                p.x * scale.x, p.y * scale.y, p.z * scale.z
            };
            const auto rotated = vk::util::math::rotate(
                object.pose.rotation, local);
            const auto world = position + rotated;
            bounds.minimum.x = std::min(bounds.minimum.x, world.x);
            bounds.minimum.y = std::min(bounds.minimum.y, world.y);
            bounds.minimum.z = std::min(bounds.minimum.z, world.z);
            bounds.maximum.x = std::max(bounds.maximum.x, world.x);
            bounds.maximum.y = std::max(bounds.maximum.y, world.y);
            bounds.maximum.z = std::max(bounds.maximum.z, world.z);
        }
        initial_scene.instances.push_back(object);
        initial_scene.collision_boxes.push_back(bounds);
    }

    // Define level geometry, instances, imported models and lights here.
    ::scene::state make_level()
    {
        ::scene::state initial_scene;
        // Checker floor with three white cubes; no surrounding test walls.
        // Floor surface stays at y=0 for gameplay collisions.

        auto floor=make_rect(1,10.f,.1f,10.f,{1.f,1.f,1.f,1.f});
        floor.surface.procedural_pattern=1;
        auto cube=make_rect(2,.5f,.5f,.5f,{1.f,1.f,1.f,1.f});
        initial_scene.meshes.push_back(std::move(floor));
        initial_scene.meshes.push_back(std::move(cube));
        place(initial_scene,1,1, {0, -0.1f, 0});
        place(initial_scene,2,2, {-2.f, .5f, 0});
        place(initial_scene,3,2, {0, .5f, 0});
        place(initial_scene,4,2, {2.f, .5f, 0});
        // Imported models: path, world position/scale, animation clip.
        ::scene::transform mermaid{};
        mermaid.position = {0.0f,1.0f,0.5f/3.0f};
        mermaid.scale = {1.5f,1.5f,1.5f};
        srv::application::load_model("assets/mermaid/mermaid.glb", mermaid, 0);

        // Level-authored lights: position, radius, RGB, intensity.
        // Keep the example light in view from the default player spawn,
        // so its Hell2024 LIGHT label is immediately selectable in ? mode.
        initial_scene.lights.push_back({
            {0.0f,3.0f,1.0f},6.0f,{1.0f,0.5f,0.2f},4.0f
        });

        return initial_scene;
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
        settings.bind_host = "0.0.0.0";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
        game::options::init(argc, argv, game::options::mode::server_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        player::configure({
            {0, 0, 4}, // spawn
            4.0f,      // walking speed
            14.0f,     // sprint speed
            20.0f,     // gravity
            0.3f,      // player radius (collision width)
            1.8f       // player height (collision and camera)
        });

        // One game-authored gun profile supplies both the server's
        // authoritative rules and the client's models, controls and clips.
        for(const auto& gun:game_weapons::all())
            srv::application::add_weapon(gun.rules);
        srv::application::on_weapon_command(
        [](srv::application::weapon_command_context& event)
        {
            const auto result = ::weapon::apply(
                event.state,
                event.command.sequence,
                event.command.kind,
                event.now,
                event.definition
            );

            if (result != ::weapon::result::fired)
                return ::weapon::hit{};

            return ::weapon::trace(event.world,
                event.player.position,
                weapon::direction(
                    event.command.yaw,
                    event.command.pitch
                ),
                event.definition.range
            );
        });

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        srv::application::set_scene(make_level());
        // The level editor is a developer-only operation. Only local DTLS
        // clients may change this server's lights; other peers can observe
        // the replicated results but cannot mutate the world.
        srv::application::on_editor_permission(
            [](const net::endpoint::resource& peer)
            {
                if(peer.address.ss_family==AF_INET)
                {
                    const auto* address=
                        reinterpret_cast<const sockaddr_in*>(&peer.address);
                    const auto* octets=reinterpret_cast<const unsigned char*>(
                        &address->sin_addr);
                    return octets[0]==127; // 127.0.0.0/8
                }
                if(peer.address.ss_family==AF_INET6)
                {
                    const auto* address=
                        reinterpret_cast<const sockaddr_in6*>(&peer.address);
                    return IN6_IS_ADDR_LOOPBACK(&address->sin6_addr)!=0;
                }
                return false;
            });

        srv::application::init();
        const int result = srv::application::run();
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-server: " << e.what() << '\n';
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



// [srv/assets/animation.cpp]

// [srv/assets/animation.hpp]

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <filesystem>
#include <vector>

namespace srv::assets
{
    // One animation track and its reference skeleton. Import/conversion to
    // .ozz is an offline operation; sampling is CPU-only and thread-confined.
    struct animation
    {
        ozz::animation::Skeleton skeleton;
        ozz::animation::Animation clip;
        ozz::animation::SamplingJob::Context context;
        std::vector<ozz::math::SoaTransform> locals;
        std::vector<ozz::math::Float4x4> matrices;

        void load(const std::filesystem::path& skeleton_file,
                  const std::filesystem::path& animation_file);
        bool sample(float seconds);
    };
} // namespace srv::assets



#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <cmath>
#include <stdexcept>

namespace srv::assets
{
    namespace
    {
        template <typename T>
        void read(const std::filesystem::path& path, T& value)
        {
            ozz::io::File file(path.string().c_str(), "rb");
            if (!file.opened())
                throw std::runtime_error("Cannot open Ozz file: " + path.string());
            ozz::io::IArchive archive(&file);
            if (!archive.TestTag<T>())
                throw std::runtime_error("Wrong Ozz asset type: " + path.string());
            archive >> value;
        }
    } // namespace

    void animation::load(const std::filesystem::path& skeleton_file,
                         const std::filesystem::path& animation_file)
    {
        read(skeleton_file, skeleton);
        read(animation_file, clip);
        if (skeleton.num_joints() != clip.num_tracks())
            throw std::runtime_error("Ozz skeleton and clip have different joint counts");
        context.Resize(skeleton.num_joints());
        locals.resize(skeleton.num_soa_joints());
        matrices.resize(skeleton.num_joints());
    }

    bool animation::sample(float seconds)
    {
        if (locals.empty() || !std::isfinite(seconds) || clip.duration() <= 0)
            return false;
        auto time = std::fmod(seconds, clip.duration());
        if (time < 0) time += clip.duration();
        ozz::animation::SamplingJob sampler;
        sampler.animation = &clip;
        sampler.context = &context;
        sampler.ratio = time / clip.duration();
        sampler.output = ozz::make_span(locals);
        if (!sampler.Run()) return false;

        ozz::animation::LocalToModelJob joints;
        joints.skeleton = &skeleton;
        joints.input = ozz::make_span(locals);
        joints.output = ozz::make_span(matrices);
        return joints.Run();
    }
} // namespace srv::assets



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



// [srv/app/application.cpp]




#include <jobs/pool.hpp>

// [srv/network/session.hpp]

#include <network/endpoint.hpp>



#include <chrono>
#include <deque>
#include <vector>

namespace srv::session
{
    struct resource
    {
        net::endpoint::resource endpoint;
        std::uint32_t token = 0;
        std::uint32_t last_sequence = 0;
        bool has_sequence = false;
        bool active = false;
        std::chrono::steady_clock::time_point last_seen;
        ::player::state avatar;

        struct owned_weapon
        {
            std::uint32_t id = 0;
            ::weapon::state state;
            std::optional<net::weapon::snapshot> published;
        };

        std::vector<owned_weapon> weapons;
        std::uint32_t active_weapon = 0;
        std::uint32_t weapon_sequence = 0;
        std::deque<net::weapon::command> weapon_commands;
        // Independent idempotent sequence for the DTLS editor channel.
        std::uint32_t editor_light_sequence=0;
        std::uint32_t editor_light_index=UINT32_MAX;

        struct pending_shot
        {
            net::weapon::shot value;
            std::uint32_t repeats = 3;
        };

        std::deque<pending_shot> shots;
        std::optional<net::weapon::shot> last_shot;
        std::deque<::player::input> inputs;
        std::uint32_t processed_input = 0;
        std::uint32_t scene_request = 0;
        bool scene_pending = false;
        std::uint32_t queued_scene_batch=UINT32_MAX;
        std::chrono::steady_clock::time_point queued_scene_at{};
    };

    struct state
    {
        std::vector<resource> clients;
    };

    state* get();
    void init();
    void expire(std::chrono::steady_clock::time_point now);
    void shutdown();
} // namespace srv::session




// [srv/shared/net/editor_light.hpp]


// [srv/shared/net/movement.hpp]



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


#include <network/endpoint.hpp>

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



#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace srv::application
{
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    namespace
    {
        jobs::pool& generation_jobs()
        {
            static jobs::pool worker;
            return worker;
        }

        void receive_messages();
        void send_editor_lights();
        void handle_editor_light(const net::endpoint::resource&,const net::editor_light::resource&);
        void send_scene_batches();
        void send_animation_states();
        void send_attachment_events();
        void handle(const net::endpoint::resource& peer, const net::message::resource& message);
        void reply(
            const net::endpoint::resource& peer,
            net::message::type type,
            const net::message::resource& request
        );
        void tick_players();
        void tick_weapons();
        void
        handle_weapon(const net::endpoint::resource& peer, const net::weapon::command& command);
        net::weapon::snapshot weapon_snapshot(
            const session::resource& client,
            const session::resource::owned_weapon& owned,
            double now
        );
    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void set_scene(::scene::state scene)
    {
        if (get()->initialized) throw std::runtime_error("Cannot replace a running scene");
        get()->scene = std::move(scene);
        get()->editor_light_versions.assign(get()->scene.lights.size(),1u);
        get()->editor_light_revision=1;
        get()->scene_configured = true;
    }

    void load_model(std::filesystem::path path, ::scene::transform pose,
                    std::uint32_t animation)
    {
        if (get()->initialized) throw std::runtime_error("Register model imports before init");
        if (path.empty() || !std::filesystem::is_regular_file(path))
            throw std::runtime_error("Model file not found: " + path.string());
        get()->models.push_back({std::move(path), pose, animation});
    }

    void add_weapon(::weapon::definition definition)
    {
        if (get()->initialized)
        {
            throw std::runtime_error("Register weapons before init");
        }
        ::weapon::add(get()->weapons,std::move(definition));
    }

    void on_weapon_command(weapon_command_handler handler)
    {
        if (get()->initialized)
        {
            throw std::runtime_error("Configure weapon handler before init");
        }
        get()->on_weapon_command = std::move(handler);
    }

    void on_editor_permission(
        std::function<bool(const net::endpoint::resource&)> handler)
    {
        if(get()->initialized)
            throw std::runtime_error("Configure editor permission before init");
        get()->editor_permission=std::move(handler);
    }

    void init()
    {
        if (get()->initialized)
        {
            throw std::runtime_error("srv::application already initialized");
        }

        if (!get()->scene_configured)
            throw std::runtime_error("The game must provide a scene with set_scene()");
        get()->options = *game::options::get();
        get()->socket = net::secure::create(
            net::secure::role::server,
            game::secure_config::load(net::secure::role::server),
            get()->options.port,get()->options.bind_host);
        net::session::settings transport_settings;
        transport_settings.channel.max_message=net::scene_stream::max_scene_bytes+20;
        transport_settings.channel.max_queued=2*(net::scene_stream::max_scene_bytes+20);
        transport_settings.channel.window=512;
        transport_settings.max_peers=net::limits::max_clients;
        transport_settings.allow_unknown_peers=false;
        get()->scene_transport=std::make_unique<net::session>(transport_settings);
        session::init();
        get()->colliders = player::build_colliders(get()->scene);
        get()->collision_index = player::build_index(get()->colliders);
        ::weapon::append(get()->weapon_world,get()->scene.meshes, get()->scene.instances);
        generation_jobs().start(1,64);
        get()->scene_batches = {{net::scene_stream::serialize(get()->scene),true}};
        get()->tick = 0;
        get()->initialized = true;

        // Import game-declared assets (including textures, skins and clips).
        // Vulkan resources remain entirely client-side.
        auto requests = std::move(get()->models);
        if (!requests.empty())
        {
            begin_stream();
            if (!generation_jobs().submit([requests = std::move(requests)]() -> jobs::completion
            {
                ::scene::state combined;
                auto mesh_id = std::uint32_t(1);
                auto instance_id = std::uint32_t(1);
                auto texture_id = std::uint32_t(1);
                auto skin_id = std::uint32_t(1);
                for (const auto& mesh : get()->scene.meshes)
                    mesh_id = std::max(mesh_id, mesh.id + 1);
                for (const auto& instance : get()->scene.instances)
                    instance_id = std::max(instance_id, instance.id + 1);
                for (const auto& texture : get()->scene.textures)
                    texture_id = std::max(texture_id, texture.id + 1);
                for (const auto& skin : get()->scene.skins)
                    skin_id = std::max(skin_id, skin.id + 1);

                for (const auto& request : requests)
                {
                    const auto import_started = clock::now();
                    auto imported = assets::load(request.path, mesh_id, instance_id,
                                                 texture_id, skin_id);
                    if (imported.meshes.empty())
                        throw std::runtime_error("Model has no renderable meshes: " + request.path.string());
                    for (auto& instance : imported.instances)
                    {
                        const auto mesh = std::find_if(imported.meshes.begin(), imported.meshes.end(),
                            [&](const ::scene::mesh& entry) { return entry.id == instance.mesh_id; });
                        if (mesh == imported.meshes.end())
                            throw std::runtime_error("Imported instance references a missing mesh");
                        if (mesh->skin_id)
                        {
                            const auto skin = std::find_if(imported.skins.begin(), imported.skins.end(),
                                [&](const ::scene::skin& entry) { return entry.id == mesh->skin_id; });
                            if (skin == imported.skins.end() ||
                                (request.animation >= skin->clips.size() &&
                                 !(skin->clips.empty() && request.animation == 0)))
                                throw std::runtime_error("Requested animation clip does not exist: " +
                                                         request.path.string());
                        }
                        instance.pose = request.pose;
                        instance.animation = request.animation;
                    }
                    std::cout << "srv: imported " << request.path.string() << " ("
                              << imported.meshes.size() << " meshes, "
                              << imported.instances.size() << " instances, "
                              << imported.textures.size() << " textures, "
                              << imported.skins.size() << " skins, "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     clock::now() - import_started).count()
                              << " ms)\n";
                    mesh_id += std::uint32_t(imported.meshes.size());
                    instance_id += std::uint32_t(imported.instances.size());
                    texture_id += std::uint32_t(imported.textures.size());
                    skin_id += std::uint32_t(imported.skins.size());
                    combined.meshes.insert(combined.meshes.end(),
                        std::make_move_iterator(imported.meshes.begin()),
                        std::make_move_iterator(imported.meshes.end()));
                    combined.instances.insert(combined.instances.end(),
                        std::make_move_iterator(imported.instances.begin()),
                        std::make_move_iterator(imported.instances.end()));
                    combined.textures.insert(combined.textures.end(),
                        std::make_move_iterator(imported.textures.begin()),
                        std::make_move_iterator(imported.textures.end()));
                    combined.skins.insert(combined.skins.end(),
                        std::make_move_iterator(imported.skins.begin()),
                        std::make_move_iterator(imported.skins.end()));
                }
                return [imported = std::make_shared<::scene::state>(std::move(combined))]() mutable
                {
                    get()->importing = std::move(imported);
                    get()->next_texture = 0;
                    get()->next_skin = 0;
                    get()->next_mesh = 0;
                    get()->next_instance = 0;
                };
            }))
                throw std::runtime_error("Could not queue model imports");
        }
    }

    void begin_stream()
    {
        auto& data = *get();
        data.scene_batches = {{net::scene_stream::serialize(data.scene),false}};
    }

    void append_scene(::scene::state patch, bool final)
    {
        auto& data = *get();
        if (!data.initialized || data.scene_batches.empty() || data.scene_batches.back().final)
        {
            throw std::runtime_error("Scene stream not accepting batches");
        }
        if (data.scene_batches.size() >= 65535)
        {
            throw std::runtime_error("Too many scene batches");
        }

        auto bytes = net::scene_stream::serialize(patch);
        if (bytes.size() > net::scene_stream::max_scene_bytes)
            throw std::runtime_error("Scene asset exceeds the network batch limit (75 MiB)");

        for (auto& skin : patch.skins)
            data.scene.skins.push_back(std::move(skin));

        for (auto& texture : patch.textures)
        {
            data.scene.textures.push_back(std::move(texture));
        }

        for (auto& mesh : patch.meshes)
        {
            data.scene.meshes.push_back(std::move(mesh));
        }

        // Only the new geometry needs collision bounds and a new BVH root.
        ::weapon::append(data.weapon_world,data.scene.meshes, patch.instances);
        auto added_colliders = player::build_colliders(data.scene.meshes, patch.instances);
        std::vector<spatial::bounds> boxes;
        boxes.reserve(added_colliders.size());
        for (const auto& collider : added_colliders)
        {
            boxes.push_back({collider.min, collider.max});
        }

        for (auto& instance : patch.instances)
        {
            data.scene.instances.push_back(std::move(instance));
        }
        for (auto& box : patch.collision_boxes)
        {
            data.scene.collision_boxes.push_back(std::move(box));
        }
        for(auto& light:patch.lights)
        {
            data.scene.lights.push_back(std::move(light));
            data.editor_light_versions.push_back(data.editor_light_revision);
        }
        data.colliders.insert(
            data.colliders.end(), added_colliders.begin(), added_colliders.end()
        );
        data.collision_index.append(boxes);
        data.scene_batches.push_back({std::move(bytes),final});
    }

    bool play_animation(std::uint32_t instance_id, std::uint32_t clip,
                        std::uint32_t overlay, float blend, std::uint32_t mask_root)
    {
        if (!std::isfinite(blend) || blend < 0.f || blend > 1.f)
            return false;
        auto& world = get()->scene;
        const auto it = std::find_if(world.instances.begin(), world.instances.end(),
            [=](const ::scene::instance& instance) { return instance.id == instance_id; });
        if (it == world.instances.end()) return false;
        const auto mesh = std::find_if(world.meshes.begin(), world.meshes.end(),
            [&](const ::scene::mesh& entry) { return entry.id == it->mesh_id; });
        if (mesh == world.meshes.end() || !mesh->skin_id) return false;
        const auto skin = std::find_if(world.skins.begin(), world.skins.end(),
            [&](const ::scene::skin& entry) { return entry.id == mesh->skin_id; });
        if (skin == world.skins.end() || clip >= skin->clips.size() ||
            (overlay != UINT32_MAX && overlay >= skin->clips.size()) ||
            (mask_root != UINT32_MAX && mask_root >= skin->inverse_bind.size()))
            return false;

        it->animation = clip;
        it->animation_tick = get()->tick;
        it->overlay_animation = overlay;
        it->overlay_weight = blend;
        it->overlay_root = mask_root;
        return true;
    }

    bool emit_attachment_event(std::uint32_t instance_id,
                               std::uint32_t joint, std::uint32_t kind)
    {
        auto& state = *get();
        if (state.attachment_events.size() >= 128) return false;
        const auto it = std::find_if(state.scene.instances.begin(),
            state.scene.instances.end(), [&](const ::scene::instance& instance)
            { return instance.id == instance_id; });
        if (it == state.scene.instances.end()) return false;
        const auto mesh = std::find_if(state.scene.meshes.begin(),
            state.scene.meshes.end(), [&](const ::scene::mesh& m)
            { return m.id == it->mesh_id; });
        if (mesh == state.scene.meshes.end() || !mesh->skin_id) return false;
        const auto skin = std::find_if(state.scene.skins.begin(),
            state.scene.skins.end(), [&](const ::scene::skin& entry)
            { return entry.id == mesh->skin_id; });
        if (skin == state.scene.skins.end() ||
            joint >= skin->inverse_bind.size()) return false;
        state.attachment_events.push_back({
            {0, state.tick, ++state.next_event, instance_id, joint, kind}, 3
        });
        return true;
    }

    int run()
    {
        if (!get()->initialized)
        {
            throw std::runtime_error("srv::application is not initialized");
        }

        const auto started = clock::now();
        auto next = started;
        const auto interval = std::chrono::nanoseconds(1'000'000'000 / net::limits::tick_hz);

        while (!game::application::get()->stop.load(std::memory_order_relaxed))
        {
            const auto now = clock::now();

            if (get()->options.duration > 0 &&
                std::chrono::duration<double>(now - started).count() >= get()->options.duration)
            {
                break;
            }
            ++get()->tick;

            session::expire(now);

            // Process input and simulation before bulk scene data. Bound the
            // amount of network work done inside one authoritative tick.
            receive_messages();
            tick_players();
            tick_weapons();
            send_editor_lights();
            send_animation_states();
            send_attachment_events();

            // Group small assets together instead of adding only one asset
            // every 60 Hz tick. Bound serialization work to a few MiB per batch.
            if (get()->importing)
            {
                auto& imported = *get()->importing;
                ::scene::state patch;
                constexpr std::size_t target_bytes = 4 * 1024 * 1024;
                std::size_t used = 20;
                auto fits = [&](std::size_t size)
                {
                    return used == 20 || size <= target_bytes - used;
                };

                while (true)
                {
                    std::size_t size = 0;
                    if (get()->next_texture < imported.textures.size())
                    {
                        auto& texture = imported.textures[get()->next_texture];
                        size = 20 + (texture.encoded.empty()
                            ? texture.pixels.size() : texture.encoded.size());
                        if (!fits(size)) break;
                        patch.textures.push_back(std::move(texture));
                        ++get()->next_texture;
                    }
                    else if (get()->next_skin < imported.skins.size())
                    {
                        auto& skin = imported.skins[get()->next_skin];
                        size = 16 + skin.skeleton.size() + skin.inverse_bind.size() * 64;
                        for (const auto& clip : skin.clips)
                            size += 8 + clip.name.size() + clip.bytes.size();
                        if (!fits(size)) break;
                        patch.skins.push_back(std::move(skin));
                        ++get()->next_skin;
                    }
                    else if (get()->next_mesh < imported.meshes.size())
                    {
                        auto& mesh = imported.meshes[get()->next_mesh];
                        size = 96 + mesh.vertices.size() * (mesh.skin_id ? 64 : 32) +
                            mesh.indices.size() * 4;
                        if (!fits(size)) break;
                        patch.meshes.push_back(std::move(mesh));
                        ++get()->next_mesh;
                    }
                    else if (get()->next_instance < imported.instances.size())
                    {
                        size = 80;
                        if (!fits(size)) break;
                        patch.instances.push_back(
                            std::move(imported.instances[get()->next_instance++]));
                    }
                    else break;

                    used += size;
                    if (used >= target_bytes) break;
                }

                const bool final =
                    get()->next_texture == imported.textures.size() &&
                    get()->next_skin == imported.skins.size() &&
                    get()->next_mesh == imported.meshes.size() &&
                    get()->next_instance == imported.instances.size();

                if (!patch.textures.empty() || !patch.skins.empty() ||
                    !patch.meshes.empty() || !patch.instances.empty())
                    append_scene(std::move(patch), final);

                if (final)
                {
                    std::size_t bytes = 0;
                    for (const auto& batch : get()->scene_batches)
                        bytes += batch.bytes.size();
                    std::cout << "srv: model stream ready: " << get()->scene_batches.size()
                              << " batches, " << bytes / 1024 << " KiB on the wire\n";
                    get()->importing.reset();
                }
            }

            send_scene_batches();
            get()->scene_transport->pump(
                [&](const net::endpoint::resource& peer,const void* bytes,std::size_t size) {
                    return net::secure::send(get()->socket,peer,bytes,size);
                },512*1200);

            // Streaming generation may be expensive; commit only when there is
            // room before the next tick, without blocking player simulation.
            if (clock::now() +
                std::chrono::milliseconds(net::limits::generation_slack_ms) <
                next + interval ||
                get()->tick % net::limits::generation_commit_ticks == 0)
            {
                generation_jobs().commit(2);
            }

            next += interval;
            const auto completed = clock::now();

            // Recover up to one missed tick on wakeup jitter rather than
            // permanently shifting the 60 Hz schedule behind real time.
            if (completed > next + interval)
            {
                next = completed - interval;
            }
            if (next > completed)
            {
                std::this_thread::sleep_until(next);
            }
        }

        return 0;
    }

    void shutdown() noexcept
    {
        get()->scene_transport.reset();
        net::secure::destroy(get()->socket);
        session::shutdown();
        generation_jobs().stop();
        get()->scene_batches.clear();
        get()->colliders.clear();
        get()->weapon_world = {};
        get()->weapons = {};
        get()->on_weapon_command = {};
        get()->editor_permission = {};
        get()->scene = {};
        get()->editor_light_versions.clear();
        get()->editor_light_revision=1;
        get()->models.clear();
        get()->scene_configured = false;
        get()->importing.reset();
        get()->next_texture = 0;
        get()->next_skin = 0;
        get()->next_mesh = 0;
        get()->next_instance = 0;
        get()->animation_cursor = 0;
        get()->attachment_events.clear();
        get()->next_event = 0;
        get()->tick = 0;
        get()->initialized = false;
    }

    namespace
    {
        void handle_editor_light(
            const net::endpoint::resource& peer,
            const net::editor_light::resource& command)
        {
            // Authenticated DTLS is not sufficient to grant editing rights.
            // The host game must explicitly authorize this peer.
            if(!get()->editor_permission || !get()->editor_permission(peer))
                return;
            // Same DTLS endpoint/token as movement and weapons.
            for(auto& client:session::get()->clients)
            {
                if(!client.active || client.token!=command.token ||
                   !net::endpoint::equal(client.endpoint,peer))
                    continue;

                const bool duplicate=command.sequence==client.editor_light_sequence;
                if(!duplicate && (!net::message::newer(
                    command.sequence,client.editor_light_sequence) ||
                    (client.editor_light_sequence &&
                     command.sequence-client.editor_light_sequence>256u)))
                    return;

                if(!duplicate)
                {
                    std::uint32_t index=command.index;
                    if(command.type==net::editor_light::kind::create)
                    {
                        // Reuse disabled slots: stable indices for every live
                        // light and no permanent 64-light cap consumption
                        // from editing/deleting the level.
                        const auto vacant=std::find_if(
                            get()->scene.lights.begin(),
                            get()->scene.lights.end(),
                            [](const ::scene::point_light& light)
                            {return !light.enabled;});
                        if(vacant!=get()->scene.lights.end())
                        {
                            index=static_cast<std::uint32_t>(
                                vacant-get()->scene.lights.begin());
                            *vacant=command.light;
                            vacant->enabled=true;
                            get()->editor_light_versions[index]=
                                ++get()->editor_light_revision;
                        }
                        else
                        {
                            if(get()->scene.lights.size()>=64)return;
                            index=static_cast<std::uint32_t>(
                                get()->scene.lights.size());
                            get()->scene.lights.push_back(command.light);
                            get()->scene.lights.back().enabled=true;
                            get()->editor_light_versions.push_back(
                                ++get()->editor_light_revision);
                        }
                    }
                    else if(command.type==net::editor_light::kind::edit)
                    {
                        if(index>=get()->scene.lights.size() ||
                           !get()->scene.lights[index].enabled ||
                           command.revision>7)return;
                        // Apply only the edited field to avoid reverting other
                        // properties with a stale client-side light copy.
                        auto& light=get()->scene.lights[index];
                        switch(command.revision)
                        {
                        case 0:light.position.x=command.light.position.x;break;
                        case 1:light.position.y=command.light.position.y;break;
                        case 2:light.position.z=command.light.position.z;break;
                        case 3:light.color.x=command.light.color.x;break;
                        case 4:light.color.y=command.light.color.y;break;
                        case 5:light.color.z=command.light.color.z;break;
                        case 6:light.radius=command.light.radius;break;
                        case 7:light.strength=command.light.strength;break;
                        }
                        get()->editor_light_versions[index]=
                            ++get()->editor_light_revision;
                    }
                    else if(command.type==net::editor_light::kind::remove)
                    {
                        if(index>=get()->scene.lights.size() ||
                           !get()->scene.lights[index].enabled)return;
                        // A tombstone avoids renumbering other lights that
                        // clients may currently be selecting/editing.
                        auto& light=get()->scene.lights[index];
                        light.enabled=false;
                        light.strength=0.0f; // No direct/indirect contribution.
                        get()->editor_light_versions[index]=
                            ++get()->editor_light_revision;
                    }
                    else return;
                    client.editor_light_sequence=command.sequence;
                    client.editor_light_index=index;

                    // Publish immediately to every connected viewport.
                    for(const auto& recipient:session::get()->clients)
                    {
                        if(!recipient.active)continue;
                        net::editor_light::resource changed{};
                        changed.type=net::editor_light::kind::state;
                        changed.token=recipient.token;
                        changed.index=index;
                        changed.sequence=get()->editor_light_versions[index];
                        changed.revision=changed.sequence;
                        changed.light=get()->scene.lights[index];
                        const auto bytes=net::editor_light::encode(changed);
                        net::secure::send(get()->socket,recipient.endpoint,
                                          bytes.data(),bytes.size());
                    }
                    std::clog<<"srv: editor light "<<index
                             <<(command.type==net::editor_light::kind::create?
                                " placed":command.type==net::editor_light::kind::remove?
                                " deleted":" updated")<<"\n";
                }
                if(client.editor_light_index>=get()->scene.lights.size())
                    return;
                // Command ACK makes creates idempotent under retransmission.
                net::editor_light::resource ack{};
                ack.type=net::editor_light::kind::ack;
                ack.token=client.token;
                ack.sequence=client.editor_light_sequence;
                ack.index=client.editor_light_index;
                ack.revision=get()->editor_light_versions[ack.index];
                ack.light=get()->scene.lights[ack.index];
                const auto bytes=net::editor_light::encode(ack);
                net::secure::send(get()->socket,peer,bytes.data(),bytes.size());
                client.last_seen=clock::now();
                return;
            }
        }

        void send_editor_lights()
        {
            // A small periodic authoritative refresh recovers lost UDP
            // notifications and brings late joiners up to date. No meshes
            // or scene data are reserialized.
            if(get()->tick%60u!=0u)return;
            for(const auto& client:session::get()->clients)
            {
                if(!client.active)continue;
                for(std::size_t i=0;i<get()->scene.lights.size();++i)
                {
                    net::editor_light::resource update{};
                    update.type=net::editor_light::kind::state;
                    update.token=client.token;
                    update.index=static_cast<std::uint32_t>(i);
                    update.sequence=i<get()->editor_light_versions.size() ?
                        get()->editor_light_versions[i]:1;
                    update.revision=update.sequence;
                    update.light=get()->scene.lights[i];
                    const auto bytes=net::editor_light::encode(update);
                    net::secure::send(get()->socket,client.endpoint,
                                      bytes.data(),bytes.size());
                }
            }
        }

        void receive_messages()
        {
            // One extra byte ensures oversized datagrams can never look like valid packets after
            // truncation.
            std::array<std::uint8_t,1473> buffer{};

            const auto deadline = clock::now() +
                std::chrono::milliseconds(net::limits::receive_ms);
            for (std::uint32_t budget = 0;
                 budget < net::limits::receive_packets && clock::now() < deadline;
                 ++budget)
            {
                net::endpoint::resource peer;
                const auto size = net::secure::receive(get()->socket, peer, buffer.data(), buffer.size());

                if (!size)
                {
                    break;
                }

                if(get()->scene_transport->accept(peer,buffer.data(),*size,clock::now()))
                    continue;
                if(const auto editor_command=net::editor_light::decode(buffer.data(),*size))
                {
                    if(editor_command->type==net::editor_light::kind::create ||
                       editor_command->type==net::editor_light::kind::edit ||
                       editor_command->type==net::editor_light::kind::remove)
                        handle_editor_light(peer,*editor_command);
                    continue;
                }
                if (const auto command = net::weapon::decode_command(buffer.data(), *size))
                {
                    handle_weapon(peer, *command);
                    continue;
                }

                if (const auto command = net::movement::decode_input(buffer.data(), *size))
                {
                    for (auto& client : session::get()->clients)
                    {
                        if (!client.active || client.token != command->token || !net::endpoint::equal(client.endpoint, peer))
                        {
                            continue;
                        }
                        
                        const auto& incoming = command->input;
                        if (!net::message::newer(incoming.sequence, client.processed_input) ||
                            incoming.sequence - client.processed_input > 256)
                        {
                            break;
                        }

                        const auto duplicate = std::find_if(client.inputs.begin(), client.inputs.end(), [&](const player::input& value){
                            return value.sequence == incoming.sequence;
                        });

                        if (duplicate == client.inputs.end())
                        {
                            // Always admit the earliest missing input. Otherwise a full
                            // out-of-order queue could permanently block reconciliation.
                            if (client.inputs.size() >= 128)
                            {
                                if (incoming.sequence != client.processed_input + 1)
                                {
                                    break;
                                }
                                client.inputs.pop_back();
                            }

                            const auto position = std::find_if(client.inputs.begin(), client.inputs.end(), [&](const player::input& value){
                                return net::message::newer(value.sequence, incoming.sequence);
                            });

                            client.inputs.insert(position, incoming);
                        }

                        client.last_seen = clock::now();
                        break;
                    }
                    
                    continue;
                }

                if (const auto message = net::message::decode(buffer.data(), *size))
                {
                    handle(peer, *message);
                }
            }
        }

        net::weapon::snapshot weapon_snapshot(
            const session::resource& client,
            const session::resource::owned_weapon& owned,
            double now
        )
        {
            const auto& value = owned.state;
            net::weapon::snapshot snapshot;
            snapshot.token = client.token;
            snapshot.sequence = client.weapon_sequence;
            snapshot.weapon_id = owned.id;
            snapshot.active_weapon_id = client.active_weapon;
            snapshot.tick = get()->tick;
            snapshot.magazine = value.magazine;
            snapshot.reserve = value.reserve;
            snapshot.reload = value.reload;
            snapshot.outcome = value.last_result;
            if (value.reload != ::weapon::reload_phase::none)
            {
                snapshot.reload_duration = float(value.reload_finishes - value.reload_started);
                snapshot.reload_elapsed =
                    std::clamp(float(now - value.reload_started), 0.0f, snapshot.reload_duration);
                snapshot.refill_time = float(value.refill_at - value.reload_started);
            }
            return snapshot;
        }

        void handle_weapon(const net::endpoint::resource& peer, const net::weapon::command& command)
        {
            for (auto& client : session::get()->clients)
            {
                if (!client.active || client.token != command.token ||
                    !net::endpoint::equal(client.endpoint, peer))
                {
                    continue;
                }
                const auto now = clock::now();
                client.last_seen = now;
                if (!net::message::newer(command.sequence, client.weapon_sequence))
                {
                    if (client.last_shot && client.last_shot->state.sequence == command.sequence &&
                        client.last_shot->state.weapon_id == command.weapon_id)
                    {
                        const auto packet = net::weapon::encode_shot(*client.last_shot);
                        net::secure::send(
                            get()->socket, client.endpoint, packet.data(), packet.size()
                        );
                    }
                    return;
                }
                // Retry queues use one shared sequence across equipment changes. Hold
                // later datagrams until missing commands arrive rather than acknowledging
                // past a dropped equip/fire and permanently discarding it on the client.
                if (command.sequence - client.weapon_sequence > 256)
                {
                    return;
                }
                const auto duplicate = std::find_if(
                    client.weapon_commands.begin(),
                    client.weapon_commands.end(),
                    [&](const net::weapon::command& value)
                    {
                        return value.sequence == command.sequence;
                    }
                );
                if (duplicate == client.weapon_commands.end())
                {
                    auto expected = client.weapon_sequence + 1;
                    if (!expected)
                    {
                        ++expected;
                    }
                    if (client.weapon_commands.size() >= 64)
                    {
                        if (command.sequence != expected)
                        {
                            return;
                        }
                        client.weapon_commands.pop_back();
                    }
                    const auto position = std::find_if(
                        client.weapon_commands.begin(),
                        client.weapon_commands.end(),
                        [&](const net::weapon::command& value)
                        {
                            return net::message::newer(value.sequence, command.sequence);
                        }
                    );
                    client.weapon_commands.insert(position, command);
                }
                const double seconds =
                    std::chrono::duration<double>(now.time_since_epoch()).count();
                while (!client.weapon_commands.empty())
                {
                    auto expected = client.weapon_sequence + 1;
                    if (!expected)
                    {
                        ++expected;
                    }
                    if (client.weapon_commands.front().sequence != expected)
                    {
                        break;
                    }
                    const auto next = client.weapon_commands.front();
                    client.weapon_commands.pop_front();
                    client.weapon_sequence = next.sequence;
                    // Outcomes describe this global acknowledgement. A different
                    // item must not retain a prior command's fired/reload outcome.
                    for (auto& item : client.weapons)
                    {
                        item.state.last_result = ::weapon::result::none;
                    }
                    const auto* definition = ::weapon::find(get()->weapons,next.weapon_id);
                    auto owned = std::find_if(
                        client.weapons.begin(),
                        client.weapons.end(),
                        [&](const session::resource::owned_weapon& value)
                        {
                            return value.id == next.weapon_id;
                        }
                    );
                    if (!definition || owned == client.weapons.end() ||
                        (next.kind != ::weapon::action::equip &&
                         client.active_weapon != next.weapon_id))
                    {
                        continue;
                    }
                    if (next.kind == ::weapon::action::equip)
                    {
                        client.active_weapon = next.weapon_id;
                        owned->state.sequence = next.sequence;
                        owned->state.last_result = ::weapon::result::equipped;
                    }
                    else if (get()->on_weapon_command)
                    {
                        weapon_command_context context{
                            next,
                            *definition,
                            owned->state,
                            client.avatar,
                            seconds,
                            get()->weapon_world
                        };
                        const auto impact = get()->on_weapon_command(context);
                        if (next.kind == ::weapon::action::fire)
                        {
                            net::weapon::shot shot{
                                weapon_snapshot(client, *owned, seconds), impact
                            };
                            client.last_shot = shot;
                            if (client.shots.size() >= 32)
                            {
                                client.shots.pop_front();
                            }
                            client.shots.push_back({shot, 3});
                        }
                    }
                }
                return;
            }
        }

        void tick_weapons()
        {
            const double now =
                std::chrono::duration<double>(clock::now().time_since_epoch()).count();
            for (auto& client : session::get()->clients)
            {
                if (!client.active)
                {
                    continue;
                }
                for (auto& owned : client.weapons)
                {
                    const auto* definition = ::weapon::find(get()->weapons,owned.id);
                    if (!definition)
                    {
                        continue;
                    }
                    ::weapon::advance(owned.state, now, *definition);
                    // Publish every active state and changes to inactive ammunition,
                    // reload phase or acknowledgement. Clients can reconcile each item
                    // independently without treating prediction as accepted authority.
                    const auto update = weapon_snapshot(client, owned, now);
                    const auto& old = owned.published;
                    const bool changed =
                        !old || old->sequence != update.sequence ||
                        old->active_weapon_id != update.active_weapon_id ||
                        old->magazine != update.magazine || old->reserve != update.reserve ||
                        old->reload != update.reload || old->outcome != update.outcome;
                    const bool refresh = old && get()->tick - old->tick >= 30;
                    if (owned.id != client.active_weapon && !changed && !refresh)
                    {
                        continue;
                    }
                    const auto snapshot = net::weapon::encode_snapshot(update);
                    net::secure::send(
                        get()->socket, client.endpoint, snapshot.data(), snapshot.size()
                    );
                    owned.published = update;
                }
                for (auto& pending : client.shots)
                {
                    const auto packet = net::weapon::encode_shot(pending.value);
                    net::secure::send(get()->socket, client.endpoint, packet.data(), packet.size());
                    --pending.repeats;
                }
                std::erase_if(
                    client.shots,
                    [](const session::resource::pending_shot& pending)
                    {
                        return !pending.repeats;
                    }
                );
            }
        }

        void handle(const net::endpoint::resource& peer, const net::message::resource& message)
        {
            session::resource* existing = nullptr;
            session::resource* available = nullptr;

            for (auto& client : session::get()->clients)
            {
                if (client.active && net::endpoint::equal(client.endpoint, peer))
                {
                    existing = &client;
                }

                if (!client.active && !available)
                {
                    available = &client;
                }
            }

            if (message.type == net::message::type::hello)
            {
                auto* client = existing ? existing : available;

                if (!client || message.token == 0)
                {
                    return;
                }

                if (!client->active || client->token != message.token)
                {
                    get()->scene_transport->close(peer);
                    *client = session::resource{};
                    client->endpoint = peer;
                    client->token = message.token;
                    client->active = true;
                    client->avatar.position = player::parameters().spawn;
                    for (const auto& definition : get()->weapons)
                    {
                        client->weapons.push_back(
                            {definition.id, ::weapon::make_state(definition), std::nullopt}
                        );
                    }
                    if (!client->weapons.empty())
                    {
                        client->active_weapon = client->weapons.front().id;
                    }
                }

                client->last_seen = clock::now();
                reply(peer, net::message::type::welcome, message);

                return;
            }

            if (!existing || existing->token != message.token)
            {
                return;
            }

            if (message.type == net::message::type::goodbye)
            {
                existing->active = false;
                get()->scene_transport->close(peer);
            }
            else if (message.type == net::message::type::ping)
            {
                if (existing->has_sequence &&
                    !net::message::newer(message.sequence, existing->last_sequence))
                {
                    return;
                }

                existing->last_sequence = message.sequence;
                existing->has_sequence = true;
                existing->last_seen = clock::now();
                reply(peer, net::message::type::state, message);
            }
            else if (message.type == net::message::type::scene_request)
            {
                existing->last_seen = clock::now();
                // Only schedule a batch; the shared reliable channel handles delivery.
                existing->scene_request = message.value;
                existing->scene_pending = true;
            }
        }

        void send_attachment_events()
        {
            auto& queue = get()->attachment_events;
            for (auto& item : queue)
            {
                for (const auto& client : session::get()->clients)
                {
                    if (!client.active) continue;
                    auto event = item.event;
                    event.token = client.token;
                    const auto bytes = net::animation::encode_event(event);
                    net::secure::send(get()->socket, client.endpoint,
                                      bytes.data(), bytes.size());
                }
                if (item.repeats) --item.repeats;
            }
            queue.erase(std::remove_if(queue.begin(), queue.end(),
                [](const state::pending_event& item) { return !item.repeats; }),
                queue.end());
        }

        void send_animation_states()
        {
            if (get()->tick % net::limits::animation_period != 0 || get()->scene.instances.empty()) return;
            const auto& instances = get()->scene.instances;
            const std::size_t count = std::min<std::size_t>(net::limits::animations_per_tick, instances.size());
            const std::size_t start = get()->animation_cursor % instances.size();
            for (auto& client : session::get()->clients)
            {
                if (!client.active) continue;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto& instance = instances[(start + i) % instances.size()];
                    const auto mesh = std::find_if(
                        get()->scene.meshes.begin(), get()->scene.meshes.end(),
                        [&](const ::scene::mesh& m) { return m.id == instance.mesh_id; });
                    if (mesh == get()->scene.meshes.end() || !mesh->skin_id)
                        continue;
                    const auto message = net::animation::encode({
                        client.token, get()->tick, instance.id, instance.animation,
                        instance.animation_tick, instance.overlay_animation,
                        instance.overlay_root, instance.overlay_weight
                    });
                    net::secure::send(
                        get()->socket, client.endpoint, message.data(), message.size());
                }
            }
            get()->animation_cursor = (start + count) % instances.size();
        }

        void send_scene_batches()
        {
            // The application selects batches and validates its session tokens.
            // The network module alone manages fragmentation, pacing and acks.
            const auto now=clock::now();
            for(auto& client:session::get()->clients)
            {
                if(!client.active||!client.scene_pending)continue;
                client.scene_pending=false;
                const auto batch_id=client.scene_request>>16;
                if(batch_id>=get()->scene_batches.size())continue;
                if(client.queued_scene_batch==batch_id &&
                   (get()->scene_transport->pending(client.endpoint)>0 ||
                    now-client.queued_scene_at<2s))
                    continue;
                const auto& batch=get()->scene_batches[batch_id];
                auto payload=net::scene_stream::encode_batch(
                    batch.bytes,client.token,batch_id,batch.final);
                if(get()->scene_transport->send(client.endpoint,std::move(payload)))
                {
                    client.queued_scene_batch=batch_id;
                    client.queued_scene_at=now;
                }
                else client.scene_pending=true;
            }
        }

        void reply(const net::endpoint::resource& peer, net::message::type type, const net::message::resource& request)
        {
            const auto data =
                net::message::encode({type, request.sequence, request.token, get()->tick});
            net::secure::send(get()->socket, peer, data.data(), data.size());
            const auto settings = net::movement::encode_parameters(
                request.token, player::parameters());
            net::secure::send(get()->socket, peer, settings.data(), settings.size());
        }

        void tick_players()
        {
            for (auto& client : session::get()->clients)
            {
                if (!client.active)
                {
                    continue;
                }

                // Apply contiguous inputs only. Never simulate a phantom input tick.
                for (std::uint32_t budget = 0; budget < net::limits::inputs_per_tick && !client.inputs.empty(); ++budget)
                {
                    if (client.inputs.front().sequence != client.processed_input + 1)
                    {
                        break;
                    }

                    const auto input = client.inputs.front();
                    client.inputs.pop_front();
                    player::step(client.avatar, input, get()->colliders, &get()->collision_index);
                    client.processed_input = input.sequence;
                }

                net::movement::snapshot snapshot;
                snapshot.token = client.token;
                snapshot.sequence = client.processed_input;
                snapshot.tick = get()->tick;
                snapshot.player = client.avatar;

                const auto packet = net::movement::encode(snapshot);

                net::secure::send(get()->socket, client.endpoint, packet.data(), packet.size());
            }
        }
    } // namespace
} // namespace srv::application



// [srv/network/session.cpp]



namespace srv::session
{
    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        *get() = {};
        get()->clients.resize(net::limits::max_clients);
    }

    void expire(std::chrono::steady_clock::time_point now)
    {
        for (auto& client : get()->clients)
        {
            if (client.active && now - client.last_seen > std::chrono::seconds(net::limits::timeout_seconds))
            {
                client.active = false;
            }
        }
    }

    void shutdown()
    {
        *get() = {};
    }
} // namespace srv::session



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

