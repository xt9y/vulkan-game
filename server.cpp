#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace net::message {
    enum class type : std::uint8_t {
hello = 1, welcome, ping, state, goodbye, scene_request
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
#include "util/math/vector.hpp"
#include <vector>
namespace scene {
    struct texture {
        std::uint32_t id = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;
        std::vector<std::uint8_t> encoded; // Original PNG/JPEG bytes for compact network transfer.
    };
    struct material {
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
        std::uint32_t procedural_pattern = 0;
    };
    inline bool same_material(const material& a, const material& b) {
return a.base_color == b.base_color && a.emissive == b.emissive && a.metallic == b.metallic && a.roughness == b.roughness && a.occlusion == b.occlusion && a.alpha_cutoff == b.alpha_cutoff &&
a.base_color_texture == b.base_color_texture && a.metallic_roughness_texture == b.metallic_roughness_texture && a.normal_texture == b.normal_texture && a.occlusion_texture == b.occlusion_texture &&
a.emissive_texture == b.emissive_texture && a.alpha_mode == b.alpha_mode && a.double_sided == b.double_sided && a.procedural_pattern == b.procedural_pattern;
    }
} // namespace scene
namespace scene {
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
    inline bool same_mesh(const mesh& a, const mesh& b) {
if (a.id != b.id || a.skin_id != b.skin_id || a.vertices.size() != b.vertices.size() || a.indices != b.indices || !same_material(a.surface, b.surface)) 
            return false;
        for (std::size_t i = 0; i < a.vertices.size(); ++i) {
            const auto& x = a.vertices[i];
            const auto& y = b.vertices[i];
if (x.position.x != y.position.x || x.position.y != y.position.y || x.position.z != y.position.z || x.normal.x != y.normal.x || x.normal.y != y.normal.y || x.normal.z != y.normal.z ||
x.u != y.u || x.v != y.v || x.joints != y.joints || x.weights != y.weights) {
                return false;
            }
        }
        return true;
    }
}; // namespace scene
#include <string>
#include <limits>
#include "util/math/quaternion.hpp"
namespace scene {
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
    struct collision_box {
        vk::util::math::vec3 minimum{};
        vk::util::math::vec3 maximum{};
    };
    struct point_light {
        vk::util::math::vec3 position{};
        float radius=1.0f;
        vk::util::math::vec3 color{1.0f,1.0f,1.0f};
        float strength=1.0f;
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
    inline bool same_instance(const instance& a, const instance& b) {
return a.id == b.id && a.mesh_id == b.mesh_id && a.animation == b.animation && a.animation_tick == b.animation_tick && a.overlay_animation == b.overlay_animation && a.overlay_root == b.overlay_root &&
a.overlay_weight == b.overlay_weight && a.pose.position.x == b.pose.position.x && a.pose.position.y == b.pose.position.y && a.pose.position.z == b.pose.position.z && a.pose.scale.x == b.pose.scale.x &&
a.pose.scale.y == b.pose.scale.y && a.pose.scale.z == b.pose.scale.z && a.pose.rotation.x == b.pose.rotation.x && a.pose.rotation.y == b.pose.rotation.y && a.pose.rotation.z == b.pose.rotation.z &&
            a.pose.rotation.w == b.pose.rotation.w;
    }
}; // namespace scene
#include <spatial/index.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
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
    inline void validate(const definition& value) {
        const auto& t = value.reload;
if (!value.id || !value.magazine_capacity || !std::isfinite(value.fire_interval) || value.fire_interval <= 0 || !std::isfinite(value.range) || value.range <= 0 ||
!std::isfinite(t.reload_duration) || !std::isfinite(t.empty_reload_duration) || !std::isfinite(t.refill_time) || !std::isfinite(t.empty_refill_time) ||
t.reload_duration <= 0 || t.empty_reload_duration <= 0 || t.refill_time < 0 || t.refill_time > t.reload_duration || t.empty_refill_time < 0 || t.empty_refill_time > t.empty_reload_duration) {
            throw std::invalid_argument("Invalid weapon definition");
        }
    }
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
    inline state make_state(const definition& settings) {
        state value;
        value.magazine = settings.magazine_capacity;
        value.reserve = settings.initial_reserve;
        return value;
    }
    inline void advance(state& value, double now, const definition& settings) {
        if (value.reload == reload_phase::none) 
            return;
        if (!value.refilled && now >= value.refill_at) {
const auto amount = std::min( settings.magazine_capacity - std::min(value.magazine, settings.magazine_capacity), value.reserve );
            value.magazine += amount;
            value.reserve -= amount;
            value.refilled = true;
        }
        if (now >= value.reload_finishes) 
            value.reload = reload_phase::none;
    }
inline result apply( state& value, std::uint32_t sequence, action command, double now, const definition& settings ) {
        advance(value, now, settings);
        if (!sequence || !net::message::newer(sequence, value.sequence)) 
            return result::none;
        value.sequence = sequence;
        if (command == action::equip) 
            return value.last_result = result::equipped;
        if (command != action::fire && command != action::reload) 
            return value.last_result = result::none;
        if (value.reload != reload_phase::none) 
            return value.last_result = result::reloading;
        if (command == action::reload) {
            if (value.magazine == settings.magazine_capacity || !value.reserve) 
                return value.last_result = result::reload_unnecessary;
            const auto& reload = settings.reload;
            const bool empty = !value.magazine;
            value.reload = empty ? reload_phase::empty : reload_phase::tactical;
            value.reload_started = now;
value.reload_finishes = now + (empty ? reload.empty_reload_duration : reload.reload_duration);
            value.refill_at = now + (empty ? reload.empty_refill_time : reload.refill_time);
            value.refilled = false;
            return value.last_result = result::reload_started;
        }
        if (now < value.next_fire) 
            return value.last_result = result::cooldown;
        if (!value.magazine) 
            return value.last_result = result::empty;
        --value.magazine;
        value.next_fire = now + settings.fire_interval;
        return value.last_result = result::fired;
    }
    inline vec3 direction(float yaw, float pitch) {
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }
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
inline void append(world& data,const std::vector<scene::mesh>& meshes, const std::vector<scene::instance>& instances) {
        using namespace vk::util::math;
        std::vector<spatial::bounds> boxes;
        for(const auto& instance:instances) {
const auto mesh=std::find_if(meshes.begin(),meshes.end(), [&](const scene::mesh& m){return m.id==instance.mesh_id;});
            if(mesh==meshes.end())continue;
            const auto transform=[&](vec3 p) {
                return instance.pose.position+rotate(instance.pose.rotation,
{p.x*instance.pose.scale.x,p.y*instance.pose.scale.y, p.z*instance.pose.scale.z});
            };
            for(std::size_t i=0;i+2<mesh->indices.size();i+=3) {
                const auto a=mesh->indices[i],b=mesh->indices[i+1],c=mesh->indices[i+2];
if(a>=mesh->vertices.size()||b>=mesh->vertices.size()|| c>=mesh->vertices.size())continue;
triangle t{transform(mesh->vertices[a].position), transform(mesh->vertices[b].position), transform(mesh->vertices[c].position), instance.id,std::uint32_t(i/3)};
                data.triangles.push_back(t);
                boxes.push_back({{
std::min({t.a.x,t.b.x,t.c.x}), std::min({t.a.y,t.b.y,t.c.y}), std::min({t.a.z,t.b.z,t.c.z})},{
std::max({t.a.x,t.b.x,t.c.x}), std::max({t.a.y,t.b.y,t.c.y}), std::max({t.a.z,t.b.z,t.c.z})}});
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
#include <filesystem>
#include <map>
#include <vk/vk.hpp>
namespace weapon {
    enum class motion {
idle,walk,draw,reload,reload_empty, ads_idle,ads_in,ads_out,ads_walk, fire1,fire2,fire3,ads_fire1,ads_fire2,ads_fire3
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
namespace game_weapons {
    ::weapon::gun make();
}
#include <fstream>
#include <iterator>
#include <regex>
namespace weapon {
inline timing load_timing( const std::filesystem::path& path, const std::string& reload_clip, const std::string& empty_reload_clip, float refill_frame, float empty_refill_frame ) {
        std::ifstream input(path);
        if (!input) 
            throw std::runtime_error("Weapon animation metadata missing: " + path.string());
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        auto clip = [&](const std::string& name, float event_frame) {
            std::string escaped;
            for (const char c : name) {
                if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos) 
                    escaped += '\\';
                escaped += c;
            }
            const std::regex pattern("\\{[^{}]*\"name\"\\s*:\\s*\"" + escaped + "\"[^{}]*\\}");
            std::smatch match;
            if (!std::regex_search(json, match, pattern)) 
                throw std::runtime_error("Weapon animation metadata missing clip " + name);
            const std::string object = match.str();
            auto number = [&](const std::string& key) {
const std::regex field( "\"" + key + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)" );
                std::smatch value;
                if (!std::regex_search(object, value, field)) 
                    throw std::runtime_error("Weapon animation metadata missing " + key);
                return std::stof(value[1].str());
            };
            const float duration = number("duration_seconds");
            const float fps = number("fps");
if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(duration) || duration <= 0 || !std::isfinite(event_frame)) 
                throw std::runtime_error("Invalid weapon animation timing");
            return std::pair{
                duration, std::max(0.0f, event_frame - number("source_first_frame")) / fps
            };
        };
const auto normal = clip(reload_clip, refill_frame), empty = clip(empty_reload_clip, empty_refill_frame);
        return {normal.first, empty.first, normal.second, empty.second};
    }
} // namespace weapon
namespace game_weapons {
    ::weapon::gun make() {
        using motion=::weapon::motion;
        ::weapon::gun glock{};
        glock.rules.id=1;
        glock.rules.magazine_capacity=15;
        glock.rules.initial_reserve=90;
        glock.rules.fire_interval=.25; 
        glock.rules.range=200;
glock.rules.reload=weapon::load_timing( "assets/glock/metadata.json", "Glock_Reload","Glock_ReloadEmpty",20,20);
        glock.input.trigger_button=0;       // left mouse button
        glock.input.aim_button=1;           // right mouse button
        glock.input.reload_key='R';
        glock.input.inspect_key='M'; // print magazine/reserve without reloading
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
        return glock;
    }
}
#include <fastgltf/types.hpp>
namespace srv::assets {
std::vector<::scene::texture> images(const fastgltf::Asset& asset, std::uint32_t first_texture);
} // namespace srv::assets
namespace net::scene_stream {
inline constexpr std::size_t max_scene_bytes=75*1024*1024;
struct batch_payload {
    std::uint32_t token=0;
    std::uint32_t batch=0;
    bool final=false;
    std::vector<std::uint8_t> scene;
};
std::vector<std::uint8_t> serialize(const ::scene::state& scene);
bool deserialize(const std::vector<std::uint8_t>& bytes,::scene::state& scene);
std::uint32_t checksum(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_batch(const std::vector<std::uint8_t>& bytes, std::uint32_t token,std::uint32_t batch,bool final);
bool decode_batch(const std::vector<std::uint8_t>& bytes,batch_payload& result);
}
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#include <fastgltf/tools.hpp>
#include <climits>
#include <span>
namespace srv::assets {
    namespace {
std::span<const std::byte> encoded(const fastgltf::Asset& asset, const fastgltf::Image& image) {
            if (const auto* array = std::get_if<fastgltf::sources::Array>(&image.data))
                return {array->bytes.data(), array->bytes.size()};
            if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
                const auto& region = asset.bufferViews.at(view->bufferViewIndex);
                const auto& buffer = asset.buffers.at(region.bufferIndex);
                if (const auto* array = std::get_if<fastgltf::sources::Array>(&buffer.data)) {
if (region.byteOffset > array->bytes.size() || region.byteLength > array->bytes.size() - region.byteOffset)
                        throw std::runtime_error("Invalid glTF image buffer view");
                    return {array->bytes.data() + region.byteOffset, region.byteLength};
                }
            }
            throw std::runtime_error("Unsupported glTF image source");
        }
    } // namespace
std::vector<::scene::texture> images(const fastgltf::Asset& asset, std::uint32_t first_texture) {
        std::vector<::scene::texture> result;
        result.reserve(asset.images.size());
        for (std::size_t i = 0; i < asset.images.size(); ++i) {
            const auto source = encoded(asset, asset.images[i]);
            if (source.empty() || source.size() > INT_MAX)
                throw std::runtime_error("Invalid glTF image size");
            int w = 0, h = 0, channels = 0;
auto* pixels = stbi_load_from_memory( reinterpret_cast<const stbi_uc*>(source.data()), int(source.size()), &w, &h, &channels, 4);
if (!pixels || w <= 0 || h <= 0 || std::uint64_t(w) * h * 4 > net::scene_stream::max_scene_bytes - 128) {
                stbi_image_free(pixels);
                throw std::runtime_error("Invalid or oversized glTF texture");
            }
            ::scene::texture texture;
            texture.id = first_texture + std::uint32_t(i);
            texture.width = std::uint32_t(w);
            texture.height = std::uint32_t(h);
texture.encoded.assign(reinterpret_cast<const std::uint8_t*>(source.data()), reinterpret_cast<const std::uint8_t*>(source.data()) + source.size());
            stbi_image_free(pixels);
            result.push_back(std::move(texture));
        }
        return result;
    }
} // namespace srv::assets
namespace srv::assets {
::scene::state load(const std::filesystem::path& path, std::uint32_t first_mesh, std::uint32_t first_instance, std::uint32_t first_texture, std::uint32_t first_skin);
} // namespace srv::assets
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>
namespace srv::assets {
    struct rig {
        ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
        std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;
        std::vector<std::string> names;
        std::vector<std::uint32_t> skin_joints; // glTF skin order -> Ozz order
    };
    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index);
::scene::skin serialize_rig( const fastgltf::Asset& asset, std::size_t skin_index, std::uint32_t id, const rig& imported );
} // namespace srv::assets
#include <fastgltf/core.hpp>
namespace srv::assets {
    namespace {
::scene::material material(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive, std::uint32_t first_texture) {
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
            const auto id = [&](std::size_t texture_index) -> std::uint32_t {
                const auto& ref = asset.textures.at(texture_index);
                if (!ref.imageIndex)
                    throw std::runtime_error("Unsupported compressed glTF texture");
                return first_texture + std::uint32_t(*ref.imageIndex);
            };
            if (source.pbrData.baseColorTexture)
                result.base_color_texture = id(source.pbrData.baseColorTexture->textureIndex);
            if (source.pbrData.metallicRoughnessTexture)
result.metallic_roughness_texture = id( source.pbrData.metallicRoughnessTexture->textureIndex);
            if (source.normalTexture)
                result.normal_texture = id(source.normalTexture->textureIndex);
            if (source.occlusionTexture)
                result.occlusion_texture = id(source.occlusionTexture->textureIndex);
            if (source.emissiveTexture)
                result.emissive_texture = id(source.emissiveTexture->textureIndex);
            return result;
        }
vk::util::math::vec3 position(const fastgltf::math::nmat4x4& m, fastgltf::math::fvec3 p) {
            const float x = p[0], y = p[1], z = p[2];
            return {
float(m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0]), float(m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1]), float(m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2])
            };
        }
vk::util::math::vec3 normal(const fastgltf::math::nmat4x4& m, fastgltf::math::fvec3 n) {
            using vec3 = vk::util::math::vec3;
            const vec3 a{float(m[0][0]), float(m[0][1]), float(m[0][2])};
            const vec3 b{float(m[1][0]), float(m[1][1]), float(m[1][2])};
            const vec3 c{float(m[2][0]), float(m[2][1]), float(m[2][2])};
            const auto x = cross(b, c);
            const auto y = cross(c, a);
            const auto z = cross(a, b);
auto transformed = (x * n[0] + y * n[1] + z * n[2]) * (dot(a, x) < 0.0f ? -1.0f : 1.0f);
            const float d = length(transformed);
            return d > 1e-8f ? transformed * (1.f / d) : vec3{0, 1, 0};
        }
    } // namespace
::scene::state load(const std::filesystem::path& path, std::uint32_t first_mesh, std::uint32_t first_instance, std::uint32_t first_texture, std::uint32_t first_skin) {
        auto bytes = fastgltf::MappedGltfFile::FromPath(path);
        if (!bytes) throw std::runtime_error("Could not open glTF: " + path.string());
        fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization);
constexpr auto options = fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages | fastgltf::Options::GenerateMeshIndices | fastgltf::Options::DecomposeNodeMatrices;
        auto parsed = parser.loadGltf(bytes.get(), path.parent_path(), options);
        if (parsed.error() != fastgltf::Error::None)
throw std::runtime_error(std::string("Invalid glTF: ") + std::string(fastgltf::getErrorMessage(parsed.error())));
        auto asset = std::move(parsed.get());
        ::scene::state result;
        result.textures = images(asset, first_texture);
        std::vector<std::vector<std::uint32_t>> remaps;
        remaps.reserve(asset.skins.size());
        for (std::size_t i = 0; i < asset.skins.size(); ++i) {
            auto imported = import_rig(asset, i);
result.skins.push_back( serialize_rig(asset, i, first_skin + std::uint32_t(i), imported) );
            remaps.push_back(std::move(imported.skin_joints));
        }
        if (asset.scenes.empty()) return result;
        const auto scene_index = asset.defaultScene.value_or(std::size_t(0));
        if (scene_index >= asset.scenes.size())
            throw std::runtime_error("Invalid glTF default scene");
fastgltf::iterateSceneNodes(asset, scene_index, fastgltf::math::nmat4x4{}, [&](fastgltf::Node& node, const fastgltf::math::nmat4x4& world) {
            if (!node.meshIndex || !node.visible) return;
            const bool skinned = node.skinIndex.has_value();
            const auto& source_mesh = asset.meshes.at(*node.meshIndex);
            for (const auto& primitive : source_mesh.primitives) {
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
if (skinned && (joints == primitive.attributes.end() || weights == primitive.attributes.end()))
                    throw std::runtime_error("Skin requires JOINTS_0 and WEIGHTS_0");
                ::scene::mesh mesh;
                mesh.id = first_mesh++;
                mesh.skin_id = skinned ? first_skin + std::uint32_t(*node.skinIndex) : 0;
                mesh.surface = material(asset, primitive, first_texture);
                mesh.vertices.resize(positions.count);
                for (std::size_t i = 0; i < positions.count; ++i) {
                    auto& vertex = mesh.vertices[i];
const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>( asset, positions, i);
vertex.position = skinned ? vk::util::math::vec3{p[0], p[1], p[2]} : position(world, p);
                    if (n != primitive.attributes.end()) {
                        const auto normal_value = fastgltf::getAccessorElement<
                            fastgltf::math::fvec3>(asset, asset.accessors[n->accessorIndex], i);
vertex.normal = skinned ? vk::util::math::vec3{normal_value[0], normal_value[1], normal_value[2]} : normal(world, normal_value);
                    } else
                        vertex.normal = {0, 1, 0};
                    if (u != primitive.attributes.end()) {
const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>( asset, asset.accessors[u->accessorIndex], i);
                        vertex.u = uv[0];
                        vertex.v = uv[1];
                    }
                    if (skinned) {
                        const auto indices = fastgltf::getAccessorElement<
fastgltf::math::u16vec4>( asset, asset.accessors[joints->accessorIndex], i);
const auto w = fastgltf::getAccessorElement<fastgltf::math::fvec4>( asset, asset.accessors[weights->accessorIndex], i);
                        const auto& remap = remaps.at(*node.skinIndex);
                        float total = 0;
                        for (int k = 0; k < 4; ++k) {
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
                if (n == primitive.attributes.end()) {
                    for (auto& vertex : mesh.vertices)
                        vertex.normal = {0, 0, 0};
                    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                        const auto a = mesh.indices[i];
                        const auto b = mesh.indices[i + 1];
                        const auto c = mesh.indices[i + 2];
if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
                            throw std::runtime_error("Invalid glTF mesh indices");
const auto face = cross( mesh.vertices[b].position - mesh.vertices[a].position, mesh.vertices[c].position - mesh.vertices[a].position );
                        mesh.vertices[a].normal = mesh.vertices[a].normal + face;
                        mesh.vertices[b].normal = mesh.vertices[b].normal + face;
                        mesh.vertices[c].normal = mesh.vertices[c].normal + face;
                    }
                    for (auto& vertex : mesh.vertices) {
                        const auto length_squared = dot(vertex.normal, vertex.normal);
vertex.normal = length_squared > 1e-12f ? vertex.normal * (1.0f / std::sqrt(length_squared)) : vk::util::math::vec3{0, 1, 0};
                    }
                }
                float low_u=std::numeric_limits<float>::infinity();
                float low_v=low_u;
                float high_u=-low_u,high_v=-low_u;
                for(const auto& vertex:mesh.vertices) {
                    low_u=std::min(low_u,vertex.u);
                    low_v=std::min(low_v,vertex.v);
                    high_u=std::max(high_u,vertex.u);
                    high_v=std::max(high_v,vertex.v);
                }
if(u==primitive.attributes.end() || (high_u-low_u<1e-6f && high_v-low_v<1e-6f)) {
                    using vec3=vk::util::math::vec3;
                    const auto component=[](vec3 p,int axis) {
                        return axis==0?p.x:(axis==1?p.y:p.z);
                    };
                    std::array<float,3> lo{
mesh.vertices.front().position.x, mesh.vertices.front().position.y, mesh.vertices.front().position.z};
                    auto hi=lo;
                    for(const auto& vertex:mesh.vertices)
                        for(int axis=0;axis<3;++axis) {
                            const auto value=component(vertex.position,axis);
                            lo[axis]=std::min(lo[axis],value);
                            hi[axis]=std::max(hi[axis],value);
                        }
                    std::array<int,3> axes{0,1,2};
                    std::sort(axes.begin(),axes.end(),[&](int a,int b) {
                        return hi[a]-lo[a]>hi[b]-lo[b];
                    });
                    for(auto& vertex:mesh.vertices) {
vertex.u=(component(vertex.position,axes[0])-lo[axes[0]])/ std::max(hi[axes[0]]-lo[axes[0]],1e-6f)*.98f+.01f;
vertex.v=(component(vertex.position,axes[1])-lo[axes[1]])/ std::max(hi[axes[1]]-lo[axes[1]],1e-6f)*.98f+.01f;
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
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
namespace srv::assets {
    namespace {
        using raw_joint = ozz::animation::offline::RawSkeleton::Joint;
        using raw_clip = ozz::animation::offline::RawAnimation;
        constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
        ozz::math::Transform transform(const fastgltf::Node& node) {
            const auto* trs = std::get_if<fastgltf::TRS>(&node.transform);
            if (!trs) throw std::runtime_error("glTF node matrix must be decomposed");
            ozz::math::Transform value{};
value.translation = {float(trs->translation[0]), float(trs->translation[1]), float(trs->translation[2])};
value.rotation = {float(trs->rotation[0]), float(trs->rotation[1]), float(trs->rotation[2]), float(trs->rotation[3])};
value.scale = {float(trs->scale[0]), float(trs->scale[1]), float(trs->scale[2])};
            return value;
        }
    } // namespace
    namespace {
        template<typename T>
        std::vector<std::uint8_t> archive(const T& value) {
            ozz::io::MemoryStream buffer;
            {
                ozz::io::OArchive writer(&buffer);
                writer << value;
            }
            std::vector<std::uint8_t> bytes(buffer.Size());
if (buffer.Seek(0, ozz::io::Stream::kSet) || buffer.Read(bytes.data(), bytes.size()) != bytes.size())
                throw std::runtime_error("Ozz archive could not be serialized");
            return bytes;
        }
    } // namespace
    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index) {
        const auto& skin = asset.skins.at(skin_index);
        const auto count = asset.nodes.size();
        std::vector<std::size_t> parent(count, none);
        for (std::size_t i = 0; i < count; ++i)
            for (const auto child : asset.nodes[i].children) {
                if (child >= count || parent[child] != none)
                    throw std::runtime_error("Invalid glTF joint hierarchy");
                parent[child] = i;
            }
        std::vector<bool> included(count, false);
        for (const auto joint : skin.joints) {
            if (joint >= count) throw std::runtime_error("Invalid skin joint index");
            auto node = joint;
            for (std::size_t steps = 0; node != none && steps <= count; ++steps) {
                included[node] = true;
                node = parent[node];
                if (steps == count)
                    throw std::runtime_error("Cyclic glTF joint hierarchy");
            }
        }
        ozz::animation::offline::RawSkeleton raw;
        std::vector<std::size_t> ordered;
        auto append = [&](std::size_t index, auto&& self) -> raw_joint {
            raw_joint joint{};
            const auto& name = asset.nodes[index].name;
joint.name = name.empty() ? ("node_" + std::to_string(index)).c_str() : name.c_str();
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
        for (auto joint : skin.joints) {
            auto it = std::find(ordered.begin(), ordered.end(), joint);
            if (it == ordered.end()) throw std::runtime_error("Skin remap missing joint");
            result.skin_joints.push_back(std::uint32_t(it - ordered.begin()));
        }
        for (const auto& source : asset.animations) {
            raw_clip clip;
            clip.name = source.name.c_str();
            clip.tracks.resize(ordered.size());
            float duration = 0.f;
            bool has_channels = false;
            for (const auto& channel : source.channels) {
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
                for (std::size_t i = 0; i < inputs.count; ++i) {
                    const auto t = fastgltf::getAccessorElement<float>(asset, inputs, i);
                    duration = std::max(duration, t);
                    if (channel.path == fastgltf::AnimationPath::Translation) {
const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>( asset, outputs, i);
                        track.translations.push_back({t, {p[0], p[1], p[2]}});
                    } else if (channel.path == fastgltf::AnimationPath::Rotation) {
const auto q = fastgltf::getAccessorElement<fastgltf::math::fvec4>( asset, outputs, i);
                        track.rotations.push_back({t, {q[0], q[1], q[2], q[3]}});
                    } else if (channel.path == fastgltf::AnimationPath::Scale) {
const auto v = fastgltf::getAccessorElement<fastgltf::math::fvec3>( asset, outputs, i);
                        track.scales.push_back({t, {v[0], v[1], v[2]}});
                    }
                }
                has_channels = true;
            }
            if (!has_channels) continue;
            clip.duration = std::max(duration, 0.001f);
            for (std::size_t i = 0; i < ordered.size(); ++i) {
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
::scene::skin serialize_rig( const fastgltf::Asset& asset, std::size_t skin_index, std::uint32_t id, const rig& imported ) {
        ::scene::skin result;
        result.id = id;
        result.skeleton = archive(*imported.skeleton);
        result.inverse_bind.resize(imported.skeleton->num_joints());
        for (auto& m : result.inverse_bind)
            m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const auto& source = asset.skins.at(skin_index);
        if (source.inverseBindMatrices) {
            const auto& accessor = asset.accessors.at(*source.inverseBindMatrices);
            if (accessor.count != imported.skin_joints.size())
                throw std::runtime_error("Mismatched glTF inverse bind matrices");
            for (std::size_t i = 0; i < accessor.count; ++i) {
const auto matrix = fastgltf::getAccessorElement<fastgltf::math::fmat4x4>( asset, accessor, i);
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
} // namespace net::animation
#include <cstring>
namespace net::animation {
    packet encode(const resource& state) {
        packet p{'A','N','I','M'};
net::movement::detail::fields(p.data()+4,1u,state.token,state.server_tick, state.instance_id,state.clip,state.start_tick,state.overlay, state.mask_root,state.blend);
        return p;
    }
    std::optional<resource> decode(const std::uint8_t* p,std::size_t n) {
if(!p||n!=packet{}.size()||p[0]!='A'||p[1]!='N'|| p[2]!='I'||p[3]!='M'||net::movement::detail::get(p+4)!=1u)
            return std::nullopt;
        resource v{};
net::movement::detail::fields(p+8,v.token,v.server_tick,v.instance_id, v.clip,v.start_tick,v.overlay,v.mask_root,v.blend);
if(!v.token||!v.instance_id||!std::isfinite(v.blend)|| v.blend<0||v.blend>1)return std::nullopt;
        return v;
    }
} // namespace net::animation
namespace net::message {
    packet encode(const resource& message) {
        packet p{'V','G','A','M',1,std::uint8_t(message.type),0,0};
net::movement::detail::fields(p.data()+8, message.sequence,message.token,message.value);
        return p;
    }
    std::optional<resource> decode(const std::uint8_t* p,std::size_t size) {
if(!p||size!=packet_size||p[0]!='V'||p[1]!='G'||p[2]!='A'|| p[3]!='M'||p[4]!=1||p[5]<1||p[5]>6||p[6]||p[7])
            return std::nullopt;
        resource v{};
        v.type=type(p[5]);
        net::movement::detail::fields(p+8,v.sequence,v.token,v.value);
        return v;
    }
    bool newer(std::uint32_t sequence,std::uint32_t previous) {
        const auto difference=sequence-previous;
        return difference!=0&&difference<0x80000000u;
    }
} // namespace net::message
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
value=(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)| (std::uint32_t(p[2])<<8)|std::uint32_t(p[3]);
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
put_all(out,m.metallic,m.roughness,m.occlusion,m.alpha_cutoff, m.base_color_texture,m.metallic_roughness_texture, m.normal_texture,m.occlusion_texture,m.emissive_texture,
            m.alpha_mode,std::uint32_t(m.double_sided),m.procedural_pattern);
    }
    bool material_take(cursor& in,::scene::material& m) {
        for(auto& x:m.base_color)if(!take(in,x))return false;
        for(auto& x:m.emissive)if(!take(in,x))return false;
        std::uint32_t double_sided=0;
if(!take_all(in,m.metallic,m.roughness,m.occlusion,m.alpha_cutoff, m.base_color_texture,m.metallic_roughness_texture,m.normal_texture, m.occlusion_texture,m.emissive_texture,m.alpha_mode,
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
return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z)&& std::isfinite(b.x)&&std::isfinite(b.y)&&std::isfinite(b.z)&& a.x<b.x&&a.y<b.y&&a.z<b.z;
    }
    bool valid_light(const ::scene::point_light& l) {
return std::isfinite(l.radius)&&l.radius>0&& std::isfinite(l.strength)&&l.strength>=0&& l.color.x>=0&&l.color.y>=0&&l.color.z>=0;
    }
}
std::vector<std::uint8_t> serialize(const ::scene::state& scene) {
if(scene.meshes.size()>max_scene_bytes/12 || scene.instances.size()>max_scene_bytes/48 ||scene.lights.size()>64)
        throw std::runtime_error("Scene snapshot exceeds maximum batch size");
    bytes out{'S','C','N','A'};
put_all(out,std::uint32_t(scene.meshes.size()), std::uint32_t(scene.instances.size()),std::uint32_t(scene.textures.size()), std::uint32_t(scene.skins.size()), std::uint32_t(scene.collision_boxes.size()),
        std::uint32_t(scene.lights.size()));
    for(const auto& t:scene.textures) {
        const bool encoded=!t.encoded.empty();
        const auto& payload=encoded?t.encoded:t.pixels;
        const auto raw=std::uint64_t(t.width)*t.height*4u;
if(!t.id||!t.width||!t.height||raw>max_scene_bytes-20|| (!encoded&&raw!=t.pixels.size())||payload.empty()|| payload.size()>max_scene_bytes-20|| out.size()+20+payload.size()>max_scene_bytes)
            throw std::runtime_error("Invalid texture or scene batch too large");
put_all(out,t.id,t.width,t.height,std::uint32_t(encoded), std::uint32_t(payload.size()));
        append(out,payload);
    }
    for(const auto& skin:scene.skins) {
if(!skin.id||skin.skeleton.empty()||skin.inverse_bind.empty()|| skin.inverse_bind.size()>1024)
            throw std::runtime_error("Invalid skeletal asset");
put_all(out,skin.id,std::uint32_t(skin.inverse_bind.size()), std::uint32_t(skin.skeleton.size()),std::uint32_t(skin.clips.size()));
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
if(m.vertices.empty()||m.indices.empty()||m.indices.size()%3|| m.vertices.size()>max_scene_bytes/64||m.indices.size()>max_scene_bytes/4)
            throw std::runtime_error("Invalid scene mesh");
put_all(out,m.id,m.skin_id,std::uint32_t(m.vertices.size()), std::uint32_t(m.indices.size()));
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
    for(const auto& i:scene.instances) 
put_all(out,i.id,i.mesh_id,i.animation,i.animation_tick, i.overlay_animation,i.overlay_root,i.overlay_weight, i.pose.position,i.pose.scale,i.pose.rotation.x, i.pose.rotation.y,i.pose.rotation.z,i.pose.rotation.w);
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
if(bytes.size()<28||bytes.size()>max_scene_bytes|| bytes[0]!='S'||bytes[1]!='C'||bytes[2]!='N'||bytes[3]!='A')
        return false;
    cursor in{bytes,4};
    std::uint32_t meshes=0,instances=0,textures=0,skins=0,boxes=0,lights=0;
if(!take_all(in,meshes,instances,textures,skins,boxes,lights)|| lights>64||lights>left(in)/32||boxes>left(in)/24|| textures>left(in)/20||skins>left(in)/16|| meshes>left(in)/12||instances>left(in)/48)return false;
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
if(!take_all(in,t.id,t.width,t.height,encoding,length)|| !t.id||!t.width||!t.height||encoding>1||length>left(in)|| std::uint64_t(t.width)*t.height*4u>max_scene_bytes-20)return false;
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
if(!take_all(in,skin.id,joints,length,clips)|| !skin.id||!joints||joints>1024||length>left(in)|| std::uint64_t(joints)*64>left(in)-length|| clips>max_scene_bytes/8)return false;
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
if(!take_all(in,mesh.id,mesh.skin_id,vertices,indices)|| !vertices||!indices||indices%3||left(in)<76|| std::uint64_t(vertices)*(mesh.skin_id?64u:32u)+ std::uint64_t(indices)*4u>left(in)-76)return false;
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
if(!take_all(in,item.id,item.mesh_id,item.animation,item.animation_tick, item.overlay_animation,item.overlay_root,item.overlay_weight, item.pose.position,item.pose.scale,item.pose.rotation.x,
item.pose.rotation.y,item.pose.rotation.z,item.pose.rotation.w)|| item.overlay_weight<0||item.overlay_weight>1)return false;
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
std::vector<std::uint8_t> encode_batch(const std::vector<std::uint8_t>& bytes, std::uint32_t token,std::uint32_t batch,bool final) {
    if(bytes.empty()||bytes.size()>max_scene_bytes||!token||batch>=65535)
        throw std::invalid_argument("invalid scene batch");
    std::vector<std::uint8_t> result{'V','G','S','C',1,std::uint8_t(final),0,0};
    put_all(result,token,batch,checksum(bytes));
    append(result,bytes);
    return result;
}
bool decode_batch(const std::vector<std::uint8_t>& bytes,batch_payload& result) {
if(bytes.size()<=20||bytes.size()>max_scene_bytes+20|| bytes[0]!='V'||bytes[1]!='G'||bytes[2]!='S'||bytes[3]!='C'|| bytes[4]!=1||bytes[5]>1||bytes[6]||bytes[7])return false;
    cursor in{bytes,8};
    std::uint32_t token=0,batch=0,expected=0;
    if(!take_all(in,token,batch,expected)||!token||batch>=65535)return false;
    const auto got=hash(bytes.data()+20,bytes.size()-20);
    if(expected!=got)return false;
result={token,batch,bytes[5]!=0, std::vector<std::uint8_t>(bytes.begin()+20,bytes.end())};
    return true;
}
} // namespace net::scene_stream
namespace net::weapon {
    using action = ::weapon::action;
    using result = ::weapon::result;
    using reload_phase = ::weapon::reload_phase;
    struct command {
        std::uint32_t token = 0, sequence = 0;
        action kind = action::fire;
        float yaw = 0, pitch = 0;
        std::uint32_t weapon_id = 0;
    };
    struct snapshot {
        std::uint32_t token = 0, sequence = 0, tick = 0;
        std::uint32_t magazine = 0, reserve = 0;
        reload_phase reload = reload_phase::none;
        result outcome = result::none;
        float reload_elapsed = 0, reload_duration = 0, refill_time = 0;
        std::uint32_t weapon_id = 0;
        std::uint32_t active_weapon_id = 0;
    };
    struct shot {
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
namespace player {
    enum button : std::uint32_t {
forward = 1u << 0, backward = 1u << 1, left = 1u << 2, right = 1u << 3, sprint = 1u << 4
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
collision_world build_colliders( const std::vector<::scene::mesh>& meshes, const std::vector<::scene::instance>& instances );
    void step(state& player, const input& input, const collision_world& world, const spatial::index* broadphase = nullptr);
} // namespace player
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
    namespace detail {
        inline void put(std::uint8_t* p,std::uint32_t n) {
            p[0]=std::uint8_t(n>>24);p[1]=std::uint8_t(n>>16);
            p[2]=std::uint8_t(n>>8);p[3]=std::uint8_t(n);
        }
        inline std::uint32_t get(const std::uint8_t* p) {
return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)| (std::uint32_t(p[2])<<8)|std::uint32_t(p[3]);
        }
        inline void put_float(std::uint8_t* p,float v) {
            std::uint32_t n;
            std::memcpy(&n,&v,4);
            put(p,n);
        }
        inline float get_float(const std::uint8_t* p) {
            const auto n=get(p);
            float v;
            std::memcpy(&v,&n,4);
            return v;
        }
        inline void write(std::uint8_t* p,std::uint32_t v){put(p,v);}
        inline void write(std::uint8_t* p,float v){put_float(p,v);}
        inline void read(const std::uint8_t* p,std::uint32_t& v){v=get(p);}
        inline void read(const std::uint8_t* p,float& v){v=get_float(p);}
        template<class... T>
        void fields(std::uint8_t* p,const T&... values) {
            ((write(p,values),p+=4),...);
        }
        template<class... T>
        void fields(const std::uint8_t* p,T&... values) {
            ((read(p,values),p+=4),...);
        }
        inline bool header(const std::uint8_t* p,std::size_t n,std::uint8_t kind) {
return p&&n>=8&&p[0]=='V'&&p[1]=='M'&&p[2]=='O'&&p[3]=='V'&& p[4]==1&&p[5]==kind&&p[6]==0&&p[7]==0;
        }
    }
    inline input_packet encode(std::uint32_t token,const ::player::input& in) {
        input_packet p{'V','M','O','V',1,1,0,0};
        detail::fields(p.data()+8,token,in.sequence,in.buttons,in.yaw);
        return p;
    }
    inline std::optional<command> decode_input(const std::uint8_t* p,std::size_t n) {
        if(n!=24||!detail::header(p,n,1))return std::nullopt;
        command v{};
        detail::fields(p+8,v.token,v.input.sequence,v.input.buttons,v.input.yaw);
if(!v.token||!v.input.sequence|| (v.input.buttons&~::player::valid_buttons)|| !std::isfinite(v.input.yaw)||std::abs(v.input.yaw)>1000.f)
            return std::nullopt;
        return v;
    }
inline parameters_packet encode_parameters(std::uint32_t token, const ::player::parameters_t& v) {
        parameters_packet p{'V','M','O','V',1,3,0,0};
detail::fields(p.data()+8,token,v.spawn.x,v.spawn.y,v.spawn.z, v.walk_speed,v.sprint_speed,v.gravity,v.radius,v.height);
        return p;
    }
inline std::optional<player_parameters> decode_parameters(const std::uint8_t* p, std::size_t n) {
        if(n!=44||!detail::header(p,n,3))return std::nullopt;
        player_parameters v{};
        auto& x=v.values;
detail::fields(p+8,v.token,x.spawn.x,x.spawn.y,x.spawn.z, x.walk_speed,x.sprint_speed,x.gravity,x.radius,x.height);
if(!v.token||!std::isfinite(x.spawn.x)||!std::isfinite(x.spawn.y)|| !std::isfinite(x.spawn.z)||!std::isfinite(x.walk_speed)|| !std::isfinite(x.sprint_speed)||!std::isfinite(x.gravity)||
!std::isfinite(x.radius)||!std::isfinite(x.height)|| x.walk_speed<=0||x.sprint_speed<=0||x.gravity<=0|| x.radius<=0||x.height<=0)return std::nullopt;
        return v;
    }
    inline state_packet encode(const snapshot& s) {
        state_packet p{'V','M','O','V',1,2,0,0};
        const auto& x=s.player;
detail::fields(p.data()+8,s.token,s.sequence,s.tick, x.position.x,x.position.y,x.position.z, x.velocity.x,x.velocity.y,x.velocity.z, std::uint32_t(x.grounded));
        return p;
    }
inline std::optional<snapshot> decode_snapshot(const std::uint8_t* p, std::size_t n) {
        if(n!=48||!detail::header(p,n,2))return std::nullopt;
        snapshot s{};
        auto& x=s.player;
        std::uint32_t flag=0;
detail::fields(p+8,s.token,s.sequence,s.tick, x.position.x,x.position.y,x.position.z, x.velocity.x,x.velocity.y,x.velocity.z,flag);
        x.grounded=flag==1;
        const auto finite=[](float v){
            return std::isfinite(v)&&std::abs(v)<1000000.f;
        };
if(!s.token||flag>1|| !finite(x.position.x)||!finite(x.position.y)||!finite(x.position.z)|| !finite(x.velocity.x)||!finite(x.velocity.y)||!finite(x.velocity.z))
            return std::nullopt;
        return s;
    }
} // namespace net::movement
namespace net::weapon {
namespace {
    using namespace net::movement::detail;
    bool header(const std::uint8_t* p,std::size_t n,std::size_t expected,std::uint8_t type) {
return p&&n==expected&&p[0]=='V'&&p[1]=='G'&&p[2]=='U'&& p[3]=='N'&&p[4]==2&&p[5]==type&&!p[6]&&!p[7];
    }
    void write_state(std::uint8_t* p,const snapshot& v) {
fields(p+8,v.token,v.sequence,v.tick,v.magazine,v.reserve, std::uint32_t(v.reload),std::uint32_t(v.outcome), v.reload_elapsed,v.reload_duration,v.refill_time, v.weapon_id,v.active_weapon_id);
    }
    std::optional<snapshot> read_state(const std::uint8_t* p) {
        snapshot v{};
        std::uint32_t reload=0,outcome=0;
fields(p+8,v.token,v.sequence,v.tick,v.magazine,v.reserve,reload,outcome, v.reload_elapsed,v.reload_duration,v.refill_time, v.weapon_id,v.active_weapon_id);
        v.reload=reload_phase(reload);
        v.outcome=result(outcome);
if(!v.token||!v.weapon_id||!v.active_weapon_id||reload>2|| outcome>std::uint32_t(result::equipped)|| !std::isfinite(v.reload_elapsed)||!std::isfinite(v.reload_duration)||
!std::isfinite(v.refill_time)||v.reload_elapsed<0|| v.reload_duration<0||v.refill_time<0|| v.reload_elapsed>v.reload_duration||v.refill_time>v.reload_duration)
            return std::nullopt;
        return v;
    }
}
command_packet encode_command(const command& v) {
    command_packet p{'V','G','U','N',2,1,0,0};
net::movement::detail::fields(p.data()+8, v.token,v.sequence,std::uint32_t(v.kind),v.yaw,v.pitch,v.weapon_id);
    return p;
}
std::optional<command> decode_command(const std::uint8_t* p,std::size_t n) {
    if(!header(p,n,command_packet{}.size(),1))return std::nullopt;
    command v{};
    std::uint32_t kind=0;
    net::movement::detail::fields(p+8,v.token,v.sequence,kind,v.yaw,v.pitch,v.weapon_id);
    v.kind=action(kind);
if(!v.token||!v.sequence||!v.weapon_id||kind<1||kind>3|| !std::isfinite(v.yaw)||!std::isfinite(v.pitch)|| std::abs(v.yaw)>1000||std::abs(v.pitch)>1.570797f)
        return std::nullopt;
    return v;
}
snapshot_packet encode_snapshot(const snapshot& v) {
    snapshot_packet p{'V','G','U','N',2,2,0,0};
    write_state(p.data(),v);
    return p;
}
std::optional<snapshot> decode_snapshot(const std::uint8_t* p,std::size_t n) {
    if(!header(p,n,snapshot_packet{}.size(),2))return std::nullopt;
    return read_state(p);
}
shot_packet encode_shot(const shot& v) {
    shot_packet p{'V','G','U','N',2,3,0,0};
    write_state(p.data(),v.state);
    const auto& h=v.impact;
net::movement::detail::fields(p.data()+56,std::uint32_t(h.impacted), h.position.x,h.position.y,h.position.z, h.normal.x,h.normal.y,h.normal.z,h.instance,h.triangle);
    return p;
}
std::optional<shot> decode_shot(const std::uint8_t* p,std::size_t n) {
    if(!header(p,n,shot_packet{}.size(),3))return std::nullopt;
    const auto state=read_state(p);
    if(!state)return std::nullopt;
    shot v{};
    v.state=*state;
    std::uint32_t impacted=0;
    auto& h=v.impact;
net::movement::detail::fields(p+56,impacted, h.position.x,h.position.y,h.position.z, h.normal.x,h.normal.y,h.normal.z,h.instance,h.triangle);
    if(impacted>1)return std::nullopt;
    h.impacted=impacted==1;
    const auto finite=[](float x){return std::isfinite(x)&&std::abs(x)<1000000.f;};
if(!finite(h.position.x)||!finite(h.position.y)||!finite(h.position.z)|| !finite(h.normal.x)||!finite(h.normal.y)||!finite(h.normal.z))
        return std::nullopt;
if(h.impacted&&(state->outcome!=result::fired||!h.instance|| std::abs(vk::util::math::length_squared(h.normal)-1.f)>.01f))
        return std::nullopt;
    return v;
}
} // namespace net::weapon
namespace net::limits {
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
#include <numeric>
namespace player {
namespace {
    using namespace vk::util::math;
    std::optional<parameters_t> configured;
    bool overlaps_xz(vec3 p,const box& b) {
        const float r=parameters().radius;
return p.x+r>b.min.x && p.x-r<b.max.x && p.z+r>b.min.z && p.z-r<b.max.z;
    }
    bool overlaps_y(float y,const box& b) {
        return y<b.max.y && y+parameters().height>b.min.y;
    }
}
void configure(parameters_t p) {
if(!std::isfinite(p.spawn.x)||!std::isfinite(p.spawn.y)|| !std::isfinite(p.spawn.z)||!std::isfinite(p.walk_speed)|| !std::isfinite(p.sprint_speed)||!std::isfinite(p.gravity)||
!std::isfinite(p.radius)||!std::isfinite(p.height)|| p.walk_speed<=0||p.sprint_speed<=0||p.gravity<=0|| p.radius<=0||p.height<=0)
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
collision_world build_colliders(const std::vector<::scene::mesh>& meshes, const std::vector<::scene::instance>& instances) {
    using namespace vk::util::math;
    collision_world boxes;
    boxes.reserve(instances.size());
    for(const auto& instance:instances) {
const auto mesh=std::find_if(meshes.begin(),meshes.end(), [&](const ::scene::mesh& m){return m.id==instance.mesh_id;});
        if(mesh==meshes.end()||mesh->vertices.empty())continue;
        const float limit=std::numeric_limits<float>::max();
        box b{{limit,limit,limit},{-limit,-limit,-limit}};
        const auto& pose=instance.pose;
        for(const auto& v:mesh->vertices) {
            const vec3 scaled{
v.position.x*pose.scale.x,v.position.y*pose.scale.y, v.position.z*pose.scale.z};
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
void step(state& p,const input& keys,const collision_world& world, const spatial::index* broadphase) {
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
    thread_local std::vector<std::uint32_t> candidates;
    candidates.clear();
    if(broadphase) {
        const float horizontal=radius+std::max(settings.walk_speed,settings.sprint_speed)*dt+.01f;
        const float vertical=std::abs(p.velocity.y)*dt+settings.gravity*dt*dt+.05f;
        broadphase->query({{
            p.position.x-horizontal,p.position.y-vertical,p.position.z-horizontal},{
p.position.x+horizontal,p.position.y+height+vertical,p.position.z+horizontal}}, [&](std::uint32_t id){candidates.push_back(id);});
    } else {
        candidates.resize(world.size());
        std::iota(candidates.begin(),candidates.end(),0u);
    }
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
if(p.velocity.y<=0 && previous.y>=b.max.y-.0001f && p.position.y<b.max.y) {
            p.position.y=b.max.y;
            p.velocity.y=0;
            p.grounded=true;
} else if(p.velocity.y>0 && previous.y+height<=b.min.y && p.position.y+height>b.min.y) {
            p.position.y=b.min.y-height;
            p.velocity.y=0;
        }
    }
}
} // namespace player
namespace game::options {
    enum class mode { server_only, client_only };
    enum class gbuffer_precision { unspecified, rgba8, rgba16f };
    enum class surface_format { unspecified, bgra8_srgb, rgba8_srgb };
    enum class depth_precision { unspecified, d32, d16 };
    enum class presentation { unspecified, fifo, mailbox, immediate };
    struct client_graphics {
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
    struct state {
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
#include <iostream>
namespace game::options {
    state* get() {
        static state value;
        return &value;
    }
    void init(int argc,char** argv,mode selected_mode,state initial) {
        initial.mode=selected_mode;
        bool host_given=false,bind_given=false;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--help") {initial.help=true;continue;}
            if(arg=="--headless") {initial.headless=true;continue;}
            if(arg!="--port"&&arg!="--duration"&&arg!="--host"&&arg!="--bind")
                throw std::invalid_argument("Unknown option: "+arg);
            if(++i==argc)throw std::invalid_argument("Missing value for "+arg);
            const std::string value=argv[i];
            std::size_t end=0;
            if(arg=="--port") {
                const auto port=std::stoul(value,&end);
                if(end!=value.size()||value.empty()||value[0]=='-'||!port||port>65535)
                    throw std::invalid_argument("Port must be in 1..65535");
                initial.port=std::uint16_t(port);
            } else if(arg=="--duration") {
                initial.duration=std::stod(value,&end);
                if(end!=value.size()||!std::isfinite(initial.duration)||initial.duration<=0)
                    throw std::invalid_argument("Duration must be a positive number of seconds");
            } else if(arg=="--host") {
                initial.host=value;
                host_given=true;
            } else {
                initial.bind_host=value;
                bind_given=true;
            }
        }
        if(!initial.help) {
            if(initial.mode==mode::server_only && initial.headless)
                throw std::invalid_argument("--headless requires a client mode");
            if(initial.mode!=mode::client_only && host_given)
                throw std::invalid_argument("--host is only supported by vulkan-client");
            if(initial.mode==mode::client_only && bind_given)
                throw std::invalid_argument("--bind is only supported by vulkan-server");
        }
if(!initial.port|| (initial.mode==mode::server_only && initial.bind_host.empty()))
            throw std::invalid_argument("A server bind address and UDP port are required");
        if(initial.mode==mode::client_only) {
            const auto& g=initial.graphics;
if(initial.host.empty()||g.width<=0||g.height<=0||g.title.empty()|| g.vertex_shader.empty()||g.fragment_shader.empty()|| g.gbuffer==gbuffer_precision::unspecified|| g.surface==surface_format::unspecified||
g.depth==depth_precision::unspecified|| g.present==presentation::unspecified|| g.culling_shader.empty()||g.lighting_shader.empty()|| g.tonemap_shader.empty()||g.fullscreen_shader.empty()||
               g.presentation_shader.empty()||g.exposure<0)
                throw std::invalid_argument("Client graphics configuration is incomplete");
        }
        *get()=std::move(initial);
    }
    void print_help() {
        const bool server=get()->mode==mode::server_only;
std::cout<<(server?"Usage: vulkan-server [OPTIONS]\n\n": "Usage: vulkan-client [OPTIONS]\n\n");
        std::cout<<"Options:\n  --port PORT         UDP port (configured in server.cpp or client.cpp)\n";
        if(server)
            std::cout<<"  --bind ADDRESS      Server listen address (server.cpp)\n";
        else
            std::cout<<"  --host HOST         Server address (client.cpp)\n"
                     <<"  --headless          Run the client without a window\n";
        std::cout<<"  --duration SECONDS  Stop automatically after the given time\n"
                 <<"  --help              Show this help\n";
    }
    void shutdown() {*get()={};}
} // namespace game::options
#include <atomic>
namespace game::application {
    struct state {
        std::atomic<bool> stop{false};
    };
    state* get();
} // namespace game::application
namespace game::application {
    state* get() {
        static state data;
        return &data;
    }
} // namespace game::application
#undef STB_IMAGE_IMPLEMENTATION
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
    inline bool valid(const ::scene::point_light& l) {
        const auto in=[](float value,float maximum) {
            return std::isfinite(value)&&value>=0&&value<=maximum;
        };
        const auto coord=[](float value) {
            return std::isfinite(value)&&std::abs(value)<=500.f;
        };
return coord(l.position.x)&&coord(l.position.y)&&coord(l.position.z)&& in(l.color.x,1)&&in(l.color.y,1)&&in(l.color.z,1)&& in(l.radius,64)&&l.radius>=.05f&&in(l.strength,20);
    }
    inline packet encode(const resource& v) {
packet p{'V','L','I','T',1,std::uint8_t(v.type), std::uint8_t(v.light.enabled?1:0),0};
        const auto& l=v.light;
net::movement::detail::fields(p.data()+8,v.token,v.sequence,v.index, v.revision,l.position.x,l.position.y,l.position.z,l.radius, l.color.x,l.color.y,l.color.z,l.strength);
        return p;
    }
    inline std::optional<resource> decode(const std::uint8_t* p,std::size_t n) {
if(!p||n!=packet{}.size()||p[0]!='V'||p[1]!='L'|| p[2]!='I'||p[3]!='T'||p[4]!=1||p[5]<1||p[5]>5|| p[6]>1||p[7]!=0)return std::nullopt;
        resource v{};
        v.type=kind(p[5]);
        auto& l=v.light;
net::movement::detail::fields(p+8,v.token,v.sequence,v.index, v.revision,l.position.x,l.position.y,l.position.z,l.radius, l.color.x,l.color.y,l.color.z,l.strength);
        l.enabled=p[6]!=0;
if(!v.token||!v.sequence||!valid(l)|| (v.type==kind::create?v.index!=UINT32_MAX:v.index>=64))
            return std::nullopt;
        return v;
    }
}
#include <network/secure.hpp>
#include <cstdlib>
namespace game::secure_config {
inline std::string env(const char* key) {
    const char* value=std::getenv(key);
    return value && *value ? value : "";
}
inline std::string storage_directory() {
    namespace fs=std::filesystem;
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
inline net::secure::settings load(net::secure::role side, const std::string& server_host={}, std::uint16_t server_port=0) {
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
#ifndef VULKAN_GAME_SHARED_ONLY
#include <network/endpoint.hpp>
#include <network/session.hpp>
#include <functional>
#include <memory>
namespace srv::application {
    struct model_request {
        std::filesystem::path path;
        ::scene::transform pose;
        std::uint32_t animation;
    };
    struct state {
        game::options::state options;
        net::secure::resource socket;
        std::unique_ptr<net::session> scene_transport;
        scene::state scene;
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
        player::collision_world colliders;
        ::weapon::world weapon_world;
        ::weapon::definition weapon{};
        bool weapon_registered=false;
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
    void set_scene(::scene::state scene);
void load_model(std::filesystem::path path, ::scene::transform pose, std::uint32_t animation);
    void add_weapon(::weapon::definition definition);
    void init();
    void begin_stream();
    void append_scene(::scene::state patch, bool final = false);
    int run();
    void shutdown() noexcept;
} // namespace srv::application
#include <network/socket.hpp>
#include <csignal>
#include <exception>
namespace {
::scene::mesh box_mesh(std::uint32_t id,vk::util::math::vec3 h, bool checker=false) {
        using vk::util::math::vec3;
        ::scene::mesh mesh{};
        mesh.id=id;
        mesh.surface.base_color={1,1,1,1};
        mesh.surface.roughness=.85f;
        mesh.surface.procedural_pattern=checker?1u:0u;
        const vec3 p[8]={
            {-h.x,-h.y,-h.z},{ h.x,-h.y,-h.z},
            { h.x, h.y,-h.z},{-h.x, h.y,-h.z},
            {-h.x,-h.y, h.z},{ h.x,-h.y, h.z},
            { h.x, h.y, h.z},{-h.x, h.y, h.z}
        };
        const int faces[6][4]={
            {4,5,6,7},{1,0,3,2},{0,4,7,3},
            {5,1,2,6},{7,6,2,3},{0,1,5,4}
        };
        const vec3 normals[6]={
            {0,0,1},{0,0,-1},{-1,0,0},
            {1,0,0},{0,1,0},{0,-1,0}
        };
        for(std::uint32_t f=0;f<6;++f) {
            const auto start=std::uint32_t(mesh.vertices.size());
            for(int j=0;j<4;++j)
                mesh.vertices.push_back({
                    p[faces[f][j]],normals[f],float(j==1||j==2),float(j>=2)
                });
            mesh.indices.insert(mesh.indices.end(),{
                start,start+1,start+2,start,start+2,start+3
            });
        }
        return mesh;
    }
    ::scene::state make_level() {
        using vk::util::math::vec3;
        ::scene::state world{};
        world.meshes.push_back(box_mesh(1,{10,.1f,10},true));
        world.meshes.push_back(box_mesh(2,{.5f,.5f,.5f}));
const auto place=[&](std::uint32_t id,std::uint32_t mesh, vec3 pos,vec3 half) {
            ::scene::instance instance{};
            instance.id=id;
            instance.mesh_id=mesh;
            instance.pose.position=pos;
            world.instances.push_back(instance);
            world.collision_boxes.push_back({pos-half,pos+half});
        };
        place(1,1,{0,-.1f,0},{10,.1f,10});
        place(2,2,{-2,.5f,0},{.5f,.5f,.5f});
        place(3,2,{0,.5f,0},{.5f,.5f,.5f});
        place(4,2,{2,.5f,0},{.5f,.5f,.5f});
        ::scene::transform mermaid{};
        mermaid.position={0,1,.5f/3.f};
        mermaid.scale={1.5f,1.5f,1.5f};
        srv::application::load_model("assets/mermaid/mermaid.glb",mermaid,0);
        world.lights.push_back({{0,3,1},6,{1,.5f,.2f},4});
        return world;
    }
    void stop(int) {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }
}
int main(int argc, char** argv) {
    try {
        game::options::state settings{};
        settings.bind_host = "0.0.0.0";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
game::options::init(argc, argv, game::options::mode::server_only, std::move(settings));
        if (game::options::get()->help) {
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
        srv::application::add_weapon(game_weapons::make().rules);
        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        srv::application::set_scene(make_level());
        srv::application::init();
        const int result = srv::application::run();
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    } catch (const std::exception& e) {
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-server: " << e.what() << '\n';
        return 1;
    }
}
#include <jobs/pool.hpp>
#include <network/endpoint.hpp>
#include <chrono>
#include <deque>
namespace srv::session {
    struct resource {
        net::endpoint::resource endpoint;
        std::uint32_t token = 0;
        std::uint32_t last_sequence = 0;
        bool has_sequence = false;
        bool active = false;
        std::chrono::steady_clock::time_point last_seen;
        ::player::state avatar;
        struct owned_weapon {
            std::uint32_t id = 0;
            ::weapon::state state;
         };
        owned_weapon weapon;
        std::uint32_t weapon_sequence = 0;
        std::deque<net::weapon::command> weapon_commands;
        std::uint32_t editor_light_sequence=0;
        std::uint32_t editor_light_index=UINT32_MAX;
        struct pending_shot {
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
    struct state {
        std::vector<resource> clients;
    };
    state* get();
    void init();
    void expire(std::chrono::steady_clock::time_point now);
    void shutdown();
} // namespace srv::session
#include <network/endpoint.hpp>
#include <network/session.hpp>
#include <thread>
namespace srv::application {
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    namespace {
        template<class T>
bool queue_input(std::deque<T>& queue,const T& item, std::uint32_t processed,std::size_t capacity) {
if(!net::message::newer(item.sequence,processed)|| item.sequence-processed>256)return false;
            if(std::any_of(queue.begin(),queue.end(),[&](const auto& value){
                return value.sequence==item.sequence;
            }))return true;
            auto expected=processed+1;
            if(!expected)++expected;
            if(queue.size()>=capacity) {
                if(item.sequence!=expected)return false;
                queue.pop_back();
            }
queue.insert(std::find_if(queue.begin(),queue.end(), [&](const auto& value) {
                    return net::message::newer(value.sequence,item.sequence);
                }),item);
            return true;
        }
        jobs::pool& generation_jobs() {
            static jobs::pool worker;
            return worker;
        }
        void receive_messages();
        void send_editor_lights();
        void handle_editor_light(const net::endpoint::resource&,const net::editor_light::resource&);
        void send_scene_batches();
        void send_animation_states();
        void handle(const net::endpoint::resource& peer, const net::message::resource& message);
void reply( const net::endpoint::resource& peer, net::message::type type, const net::message::resource& request );
        void tick_players();
        void tick_weapons();
        void
        handle_weapon(const net::endpoint::resource& peer, const net::weapon::command& command);
net::weapon::snapshot weapon_snapshot( const session::resource& client, const session::resource::owned_weapon& owned, double now );
    } // namespace
    state* get() {
        static state data;
        return &data;
    }
    void set_scene(::scene::state scene) {
        if (get()->initialized) throw std::runtime_error("Cannot replace a running scene");
        get()->scene = std::move(scene);
        get()->editor_light_versions.assign(get()->scene.lights.size(),1u);
        get()->editor_light_revision=1;
        get()->scene_configured = true;
    }
void load_model(std::filesystem::path path, ::scene::transform pose, std::uint32_t animation) {
        if (get()->initialized) throw std::runtime_error("Register model imports before init");
        if (path.empty() || !std::filesystem::is_regular_file(path))
            throw std::runtime_error("Model file not found: " + path.string());
        get()->models.push_back({std::move(path), pose, animation});
    }
    void add_weapon(::weapon::definition definition) {
        if (get()->initialized) 
            throw std::runtime_error("Register weapons before init");
        if(get()->weapon_registered)
            throw std::logic_error("Only one weapon belongs to this game");
        ::weapon::validate(definition);
        get()->weapon=std::move(definition);
        get()->weapon_registered=true;
    }
    void init() {
        if (get()->initialized) 
            throw std::runtime_error("srv::application already initialized");
        if (!get()->scene_configured)
            throw std::runtime_error("The game must provide a scene with set_scene()");
        get()->options = *game::options::get();
get()->socket = net::secure::create( net::secure::role::server, game::secure_config::load(net::secure::role::server), get()->options.port,get()->options.bind_host);
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
        auto requests = std::move(get()->models);
        if (!requests.empty()) {
            begin_stream();
            if (!generation_jobs().submit([requests = std::move(requests)]() -> jobs::completion {
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
                for (const auto& request : requests) {
                    const auto import_started = clock::now();
auto imported = assets::load(request.path, mesh_id, instance_id, texture_id, skin_id);
                    if (imported.meshes.empty())
                        throw std::runtime_error("Model has no renderable meshes: " + request.path.string());
                    for (auto& instance : imported.instances) {
const auto mesh = std::find_if(imported.meshes.begin(), imported.meshes.end(), [&](const ::scene::mesh& entry) { return entry.id == instance.mesh_id; });
                        if (mesh == imported.meshes.end())
                            throw std::runtime_error("Imported instance references a missing mesh");
                        if (mesh->skin_id) {
const auto skin = std::find_if(imported.skins.begin(), imported.skins.end(), [&](const ::scene::skin& entry) { return entry.id == mesh->skin_id; });
if (skin == imported.skins.end() || (request.animation >= skin->clips.size() && !(skin->clips.empty() && request.animation == 0)))
throw std::runtime_error("Requested animation clip does not exist: " + request.path.string());
                        }
                        instance.pose = request.pose;
                        instance.animation = request.animation;
                    }
                    std::cout << "srv: imported " << request.path.string() << " ("
                              << imported.meshes.size() << " meshes, "
                              << imported.instances.size() << " instances, "
                              << imported.textures.size() << " textures, "
                              << imported.skins.size() << " skins, "
<< std::chrono::duration_cast<std::chrono::milliseconds>( clock::now() - import_started).count()
                              << " ms)\n";
                    mesh_id += std::uint32_t(imported.meshes.size());
                    instance_id += std::uint32_t(imported.instances.size());
                    texture_id += std::uint32_t(imported.textures.size());
                    skin_id += std::uint32_t(imported.skins.size());
combined.meshes.insert(combined.meshes.end(), std::make_move_iterator(imported.meshes.begin()), std::make_move_iterator(imported.meshes.end()));
combined.instances.insert(combined.instances.end(), std::make_move_iterator(imported.instances.begin()), std::make_move_iterator(imported.instances.end()));
combined.textures.insert(combined.textures.end(), std::make_move_iterator(imported.textures.begin()), std::make_move_iterator(imported.textures.end()));
combined.skins.insert(combined.skins.end(), std::make_move_iterator(imported.skins.begin()), std::make_move_iterator(imported.skins.end()));
                }
                return [imported = std::make_shared<::scene::state>(std::move(combined))]() mutable {
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
    void begin_stream() {
        auto& data = *get();
        data.scene_batches = {{net::scene_stream::serialize(data.scene),false}};
    }
    void append_scene(::scene::state patch, bool final) {
        auto& data = *get();
        if (!data.initialized || data.scene_batches.empty() || data.scene_batches.back().final) 
            throw std::runtime_error("Scene stream not accepting batches");
        if (data.scene_batches.size() >= 65535) 
            throw std::runtime_error("Too many scene batches");
        auto bytes = net::scene_stream::serialize(patch);
        if (bytes.size() > net::scene_stream::max_scene_bytes)
            throw std::runtime_error("Scene asset exceeds the network batch limit (75 MiB)");
        for (auto& skin : patch.skins)
            data.scene.skins.push_back(std::move(skin));
        for (auto& texture : patch.textures) 
            data.scene.textures.push_back(std::move(texture));
        for (auto& mesh : patch.meshes) 
            data.scene.meshes.push_back(std::move(mesh));
        ::weapon::append(data.weapon_world,data.scene.meshes, patch.instances);
        auto added_colliders = player::build_colliders(data.scene.meshes, patch.instances);
        std::vector<spatial::bounds> boxes;
        boxes.reserve(added_colliders.size());
        for (const auto& collider : added_colliders) {
            boxes.push_back({collider.min, collider.max});
        }
        for (auto& instance : patch.instances) 
            data.scene.instances.push_back(std::move(instance));
        for (auto& box : patch.collision_boxes) 
            data.scene.collision_boxes.push_back(std::move(box));
        for(auto& light:patch.lights) {
            data.scene.lights.push_back(std::move(light));
            data.editor_light_versions.push_back(data.editor_light_revision);
        }
data.colliders.insert( data.colliders.end(), added_colliders.begin(), added_colliders.end() );
        data.collision_index.append(boxes);
        data.scene_batches.push_back({std::move(bytes),final});
    }
    int run() {
        if (!get()->initialized) 
            throw std::runtime_error("srv::application is not initialized");
        const auto started = clock::now();
        auto next = started;
        const auto interval = std::chrono::nanoseconds(1'000'000'000 / net::limits::tick_hz);
        while (!game::application::get()->stop.load(std::memory_order_relaxed)) {
            const auto now = clock::now();
if (get()->options.duration > 0 && std::chrono::duration<double>(now - started).count() >= get()->options.duration) 
                break;
            ++get()->tick;
            session::expire(now);
            receive_messages();
            tick_players();
            tick_weapons();
            send_editor_lights();
            send_animation_states();
            if (get()->importing) {
                auto& imported = *get()->importing;
                ::scene::state patch;
                constexpr std::size_t target_bytes = 4 * 1024 * 1024;
                std::size_t used = 20;
                auto fits = [&](std::size_t size) {
                    return used == 20 || size <= target_bytes - used;
                };
                while (true) {
                    std::size_t size = 0;
                    if (get()->next_texture < imported.textures.size()) {
                        auto& texture = imported.textures[get()->next_texture];
size = 20 + (texture.encoded.empty() ? texture.pixels.size() : texture.encoded.size());
                        if (!fits(size)) break;
                        patch.textures.push_back(std::move(texture));
                        ++get()->next_texture;
                    } else if (get()->next_skin < imported.skins.size()) {
                        auto& skin = imported.skins[get()->next_skin];
                        size = 16 + skin.skeleton.size() + skin.inverse_bind.size() * 64;
                        for (const auto& clip : skin.clips)
                            size += 8 + clip.name.size() + clip.bytes.size();
                        if (!fits(size)) break;
                        patch.skins.push_back(std::move(skin));
                        ++get()->next_skin;
                    } else if (get()->next_mesh < imported.meshes.size()) {
                        auto& mesh = imported.meshes[get()->next_mesh];
size = 96 + mesh.vertices.size() * (mesh.skin_id ? 64 : 32) + mesh.indices.size() * 4;
                        if (!fits(size)) break;
                        patch.meshes.push_back(std::move(mesh));
                        ++get()->next_mesh;
                    } else if (get()->next_instance < imported.instances.size()) {
                        size = 80;
                        if (!fits(size)) break;
patch.instances.push_back( std::move(imported.instances[get()->next_instance++]));
                    } else break;
                    used += size;
                    if (used >= target_bytes) break;
                }
const bool final = get()->next_texture == imported.textures.size() && get()->next_skin == imported.skins.size() && get()->next_mesh == imported.meshes.size() && get()->next_instance == imported.instances.size();
if (!patch.textures.empty() || !patch.skins.empty() || !patch.meshes.empty() || !patch.instances.empty())
                    append_scene(std::move(patch), final);
                if (final) {
                    std::size_t bytes = 0;
                    for (const auto& batch : get()->scene_batches)
                        bytes += batch.bytes.size();
                    std::cout << "srv: model stream ready: " << get()->scene_batches.size()
                              << " batches, " << bytes / 1024 << " KiB on the wire\n";
                    get()->importing.reset();
                }
            }
            send_scene_batches();
get()->scene_transport->pump( [&](const net::endpoint::resource& peer,const void* bytes,std::size_t size) {
                    return net::secure::send(get()->socket,peer,bytes,size);
                },512*1200);
if (clock::now() + std::chrono::milliseconds(net::limits::generation_slack_ms) < next + interval || get()->tick % net::limits::generation_commit_ticks == 0) 
                generation_jobs().commit(2);
            next += interval;
            const auto completed = clock::now();
            if (completed > next + interval) 
                next = completed - interval;
            if (next > completed) 
                std::this_thread::sleep_until(next);
        }
        return 0;
    }
    void shutdown() noexcept {
        get()->scene_transport.reset();
        net::secure::destroy(get()->socket);
        session::shutdown();
        generation_jobs().stop();
        get()->scene_batches.clear();
        get()->colliders.clear();
        get()->weapon_world = {};
        get()->weapon={};
        get()->weapon_registered=false;
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
        get()->tick = 0;
        get()->initialized = false;
    }
    namespace {
void handle_editor_light( const net::endpoint::resource& peer, const net::editor_light::resource& command) {
            bool local=false;
            if(peer.address.ss_family==AF_INET) {
                const auto* addr=reinterpret_cast<const sockaddr_in*>(&peer.address);
                local=reinterpret_cast<const unsigned char*>(&addr->sin_addr)[0]==127;
            } else if(peer.address.ss_family==AF_INET6) {
                const auto* addr=reinterpret_cast<const sockaddr_in6*>(&peer.address);
                local=IN6_IS_ADDR_LOOPBACK(&addr->sin6_addr)!=0;
            }
            if(!local) return;
            for(auto& client:session::get()->clients) {
if(!client.active || client.token!=command.token || !net::endpoint::equal(client.endpoint,peer))
                    continue;
                const bool duplicate=command.sequence==client.editor_light_sequence;
if(!duplicate && (!net::message::newer( command.sequence,client.editor_light_sequence) || (client.editor_light_sequence && command.sequence-client.editor_light_sequence>256u)))
                    return;
                if(!duplicate) {
                    std::uint32_t index=command.index;
                    if(command.type==net::editor_light::kind::create) {
const auto vacant=std::find_if( get()->scene.lights.begin(), get()->scene.lights.end(), [](const ::scene::point_light& light)
                            {return !light.enabled;});
                        if(vacant!=get()->scene.lights.end()) {
index=static_cast<std::uint32_t>( vacant-get()->scene.lights.begin());
                            *vacant=command.light;
                            vacant->enabled=true;
get()->editor_light_versions[index]= ++get()->editor_light_revision;
                        } else {
                            if(get()->scene.lights.size()>=64)return;
index=static_cast<std::uint32_t>( get()->scene.lights.size());
                            get()->scene.lights.push_back(command.light);
                            get()->scene.lights.back().enabled=true;
get()->editor_light_versions.push_back( ++get()->editor_light_revision);
                        }
                    } else if(command.type==net::editor_light::kind::edit) {
if(index>=get()->scene.lights.size() || !get()->scene.lights[index].enabled || command.revision>7)return;
                        auto& light=get()->scene.lights[index];
*net::editor_light::property_fields(light)[command.revision]= *net::editor_light::property_fields(command.light)[command.revision];
get()->editor_light_versions[index]= ++get()->editor_light_revision;
                    } else if(command.type==net::editor_light::kind::remove) {
if(index>=get()->scene.lights.size() || !get()->scene.lights[index].enabled)return;
                        auto& light=get()->scene.lights[index];
                        light.enabled=false;
                        light.strength=0.0f; // No direct/indirect contribution.
get()->editor_light_versions[index]= ++get()->editor_light_revision;
                    } else return;
                    client.editor_light_sequence=command.sequence;
                    client.editor_light_index=index;
                    for(const auto& recipient:session::get()->clients) {
                        if(!recipient.active)continue;
                        net::editor_light::resource changed{};
                        changed.type=net::editor_light::kind::state;
                        changed.token=recipient.token;
                        changed.index=index;
                        changed.sequence=get()->editor_light_versions[index];
                        changed.revision=changed.sequence;
                        changed.light=get()->scene.lights[index];
                        const auto bytes=net::editor_light::encode(changed);
net::secure::send(get()->socket,recipient.endpoint, bytes.data(),bytes.size());
                    }
                    std::clog<<"srv: editor light "<<index
<<(command.type==net::editor_light::kind::create? " placed":command.type==net::editor_light::kind::remove? " deleted":" updated")<<"\n";
                }
                if(client.editor_light_index>=get()->scene.lights.size())
                    return;
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
        void send_editor_lights() {
            if(get()->tick%60u!=0u)return;
            for(const auto& client:session::get()->clients) {
                if(!client.active)continue;
                for(std::size_t i=0;i<get()->scene.lights.size();++i) {
                    net::editor_light::resource update{};
                    update.type=net::editor_light::kind::state;
                    update.token=client.token;
                    update.index=static_cast<std::uint32_t>(i);
update.sequence=i<get()->editor_light_versions.size() ? get()->editor_light_versions[i]:1;
                    update.revision=update.sequence;
                    update.light=get()->scene.lights[i];
                    const auto bytes=net::editor_light::encode(update);
net::secure::send(get()->socket,client.endpoint, bytes.data(),bytes.size());
                }
            }
        }
        void receive_messages() {
            std::array<std::uint8_t,1473> buffer{};
const auto deadline = clock::now() + std::chrono::milliseconds(net::limits::receive_ms);
            for (std::uint32_t budget = 0;
                 budget < net::limits::receive_packets && clock::now() < deadline;
                 ++budget) {
                net::endpoint::resource peer;
                const auto size = net::secure::receive(get()->socket, peer, buffer.data(), buffer.size());
                if (!size) 
                    break;
                if(get()->scene_transport->accept(peer,buffer.data(),*size,clock::now()))
                    continue;
                if(const auto editor_command=net::editor_light::decode(buffer.data(),*size)) {
if(editor_command->type==net::editor_light::kind::create || editor_command->type==net::editor_light::kind::edit || editor_command->type==net::editor_light::kind::remove)
                        handle_editor_light(peer,*editor_command);
                    continue;
                }
                if (const auto command = net::weapon::decode_command(buffer.data(), *size)) {
                    handle_weapon(peer, *command);
                    continue;
                }
                if (const auto command = net::movement::decode_input(buffer.data(), *size)) {
                    for (auto& client : session::get()->clients) {
                        if (!client.active || client.token != command->token || !net::endpoint::equal(client.endpoint, peer)) 
                            continue;
if(!queue_input(client.inputs,command->input, client.processed_input,128))break;
                        client.last_seen = clock::now();
                        break;
                    }
                    continue;
                }
                if (const auto message = net::message::decode(buffer.data(), *size)) 
                    handle(peer, *message);
            }
        }
net::weapon::snapshot weapon_snapshot( const session::resource& client, const session::resource::owned_weapon& owned, double now ) {
            const auto& value = owned.state;
            net::weapon::snapshot snapshot;
            snapshot.token = client.token;
            snapshot.sequence = client.weapon_sequence;
            snapshot.weapon_id = owned.id;
            snapshot.active_weapon_id = client.weapon.id;
            snapshot.tick = get()->tick;
            snapshot.magazine = value.magazine;
            snapshot.reserve = value.reserve;
            snapshot.reload = value.reload;
            snapshot.outcome = value.last_result;
            if (value.reload != ::weapon::reload_phase::none) {
                snapshot.reload_duration = float(value.reload_finishes - value.reload_started);
snapshot.reload_elapsed = std::clamp(float(now - value.reload_started), 0.0f, snapshot.reload_duration);
                snapshot.refill_time = float(value.refill_at - value.reload_started);
            }
            return snapshot;
        }
void handle_weapon(const net::endpoint::resource& peer, const net::weapon::command& command) {
            for (auto& client : session::get()->clients) {
if (!client.active || client.token!=command.token || !net::endpoint::equal(client.endpoint,peer)) continue;
                const auto now=clock::now();
                client.last_seen=now;
                if (!net::message::newer(command.sequence,client.weapon_sequence)) {
if (client.last_shot && client.last_shot->state.sequence==command.sequence && client.last_shot->state.weapon_id==command.weapon_id) {
                        const auto bytes=net::weapon::encode_shot(*client.last_shot);
net::secure::send(get()->socket,client.endpoint, bytes.data(),bytes.size());
                    }
                    return;
                }
                auto& queue=client.weapon_commands;
                if(!queue_input(queue,command,client.weapon_sequence,64))return;
const double seconds=std::chrono::duration<double>( now.time_since_epoch()).count();
                while (!queue.empty()) {
                    auto expected=client.weapon_sequence+1;
                    if (!expected) ++expected;
                    if (queue.front().sequence!=expected) break;
                    const auto cmd=queue.front();
                    queue.pop_front();
                    client.weapon_sequence=cmd.sequence;
                    client.weapon.state.last_result=::weapon::result::none;
                    if (cmd.weapon_id!=client.weapon.id) continue;
const auto* definition= get()->weapon_registered && get()->weapon.id==cmd.weapon_id
                            ? &get()->weapon : nullptr;
                    if (!definition) continue;
                    if (cmd.kind==::weapon::action::equip) {
                        client.weapon.state.sequence=cmd.sequence;
                        client.weapon.state.last_result=::weapon::result::equipped;
                    } else {
const auto result=::weapon::apply( client.weapon.state,cmd.sequence,cmd.kind, seconds,*definition);
                        if (cmd.kind==::weapon::action::fire) {
                            const auto impact=result==::weapon::result::fired
? ::weapon::trace( get()->weapon_world,client.avatar.position, ::weapon::direction(cmd.yaw,cmd.pitch), definition->range)
                                : ::weapon::hit{};
                            net::weapon::shot shot{
                                weapon_snapshot(client,client.weapon,seconds),impact
                            };
                            client.last_shot=shot;
                            if (client.shots.size()>=32) client.shots.pop_front();
                            client.shots.push_back({shot,3});
                        }
                    }
                }
                return;
            }
        }
        void tick_weapons() {
const double now=std::chrono::duration<double>( clock::now().time_since_epoch()).count();
            for (auto& client : session::get()->clients) {
                if (!client.active) continue;
                auto& owned=client.weapon;
if (const auto* definition= get()->weapon_registered && get()->weapon.id==owned.id ? &get()->weapon : nullptr) {
                    ::weapon::advance(owned.state,now,*definition);
                    const auto state=weapon_snapshot(client,owned,now);
                    const auto bytes=net::weapon::encode_snapshot(state);
net::secure::send(get()->socket,client.endpoint, bytes.data(),bytes.size());
                }
                for (auto& pending : client.shots) {
                    const auto bytes=net::weapon::encode_shot(pending.value);
net::secure::send(get()->socket,client.endpoint, bytes.data(),bytes.size());
                    --pending.repeats;
                }
                std::erase_if(client.shots,[](const auto& shot){
                    return shot.repeats==0;
                });
            }
        }
        void handle(const net::endpoint::resource& peer, const net::message::resource& message) {
            session::resource* existing = nullptr;
            session::resource* available = nullptr;
            for (auto& client : session::get()->clients) {
                if (client.active && net::endpoint::equal(client.endpoint, peer)) 
                    existing = &client;
                if (!client.active && !available) 
                    available = &client;
            }
            if (message.type == net::message::type::hello) {
                auto* client = existing ? existing : available;
                if (!client || message.token == 0) 
                    return;
                if (!client->active || client->token != message.token) {
                    get()->scene_transport->close(peer);
                    *client = session::resource{};
                    client->endpoint = peer;
                    client->token = message.token;
                    client->active = true;
                    client->avatar.position = player::parameters().spawn;
                    if(get()->weapon_registered)
                        client->weapon={
get()->weapon.id, ::weapon::make_state(get()->weapon)
                        };
                }
                client->last_seen = clock::now();
                reply(peer, net::message::type::welcome, message);
                return;
            }
            if (!existing || existing->token != message.token) 
                return;
            if (message.type == net::message::type::goodbye) {
                existing->active = false;
                get()->scene_transport->close(peer);
            } else if (message.type == net::message::type::ping) {
if (existing->has_sequence && !net::message::newer(message.sequence, existing->last_sequence)) 
                    return;
                existing->last_sequence = message.sequence;
                existing->has_sequence = true;
                existing->last_seen = clock::now();
                reply(peer, net::message::type::state, message);
            } else if (message.type == net::message::type::scene_request) {
                existing->last_seen = clock::now();
                existing->scene_request = message.value;
                existing->scene_pending = true;
            }
        }
        void send_animation_states() {
            if (get()->tick % net::limits::animation_period != 0 || get()->scene.instances.empty()) return;
            const auto& instances = get()->scene.instances;
            const std::size_t count = std::min<std::size_t>(net::limits::animations_per_tick, instances.size());
            const std::size_t start = get()->animation_cursor % instances.size();
            for (auto& client : session::get()->clients) {
                if (!client.active) continue;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& instance = instances[(start + i) % instances.size()];
const auto mesh = std::find_if( get()->scene.meshes.begin(), get()->scene.meshes.end(), [&](const ::scene::mesh& m) { return m.id == instance.mesh_id; });
                    if (mesh == get()->scene.meshes.end() || !mesh->skin_id)
                        continue;
                    const auto message = net::animation::encode({
client.token, get()->tick, instance.id, instance.animation, instance.animation_tick, instance.overlay_animation, instance.overlay_root, instance.overlay_weight
                    });
net::secure::send( get()->socket, client.endpoint, message.data(), message.size());
                }
            }
            get()->animation_cursor = (start + count) % instances.size();
        }
        void send_scene_batches() {
            const auto now=clock::now();
            for(auto& client:session::get()->clients) {
                if(!client.active||!client.scene_pending)continue;
                client.scene_pending=false;
                const auto batch_id=client.scene_request>>16;
                if(batch_id>=get()->scene_batches.size())continue;
if(client.queued_scene_batch==batch_id && (get()->scene_transport->pending(client.endpoint)>0 || now-client.queued_scene_at<2s))
                    continue;
                const auto& batch=get()->scene_batches[batch_id];
auto payload=net::scene_stream::encode_batch( batch.bytes,client.token,batch_id,batch.final);
                if(get()->scene_transport->send(client.endpoint,std::move(payload))) {
                    client.queued_scene_batch=batch_id;
                    client.queued_scene_at=now;
                } else client.scene_pending=true;
            }
        }
        void reply(const net::endpoint::resource& peer, net::message::type type, const net::message::resource& request) {
const auto data = net::message::encode({type, request.sequence, request.token, get()->tick});
            net::secure::send(get()->socket, peer, data.data(), data.size());
const auto settings = net::movement::encode_parameters( request.token, player::parameters());
            net::secure::send(get()->socket, peer, settings.data(), settings.size());
        }
        void tick_players() {
            for (auto& client : session::get()->clients) {
                if (!client.active) 
                    continue;
                for (std::uint32_t budget = 0; budget < net::limits::inputs_per_tick && !client.inputs.empty(); ++budget) {
                    if (client.inputs.front().sequence != client.processed_input + 1) 
                        break;
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
namespace srv::session {
    state* get() {
        static state data;
        return &data;
    }
    void init() {
        *get() = {};
        get()->clients.resize(net::limits::max_clients);
    }
    void expire(std::chrono::steady_clock::time_point now) {
        for (auto& client : get()->clients) {
            if (client.active && now - client.last_seen > std::chrono::seconds(net::limits::timeout_seconds)) 
                client.active = false;
        }
    }
    void shutdown() {
        *get() = {};
    }
} // namespace srv::session
#endif
