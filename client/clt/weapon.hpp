#pragma once
#include "../../server/scene.hpp"
#include "../../server/srv/assets.hpp"
#include "../../server/weapon.hpp"
#include "animation.hpp"
#include "camera.hpp"
#include "connection.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <utility>
#include <vector>
#include <vk/vk.hpp>

namespace clt::weapon
{
using motion = ::weapon::motion;
using clip = ::weapon::clip;
using bindings = ::weapon::bindings;
using definition = ::weapon::gun;
using gun = ::weapon::gun;
bindings active_bindings();
void register_weapon(definition value);
void check_magazine();
struct instance_data
{
    vk::util::math::mat4 model;
    vk::util::math::mat4 normal;
    vk::util::math::vec4 tint{1, 1, 1, -1};
};

struct draw
{
    std::uint32_t mesh_id = 0;

    bool viewmodel = false;

    std::vector<instance_data> instances;
    std::vector<animation::matrix> palette;
};

void controls(bool trigger, bool ads, bool reload, std::uint32_t movement);
void initialize();
const ::scene::state &assets();
const std::vector<draw> &update(const vk::util::math::mat4 &view);
vk::util::math::mat4 view_projection(float aspect, const vk::util::math::mat4 &view);
void shutdown();

using namespace vk::util::math;
namespace
{
using clock = std::chrono::steady_clock;

constexpr std::uint32_t asset_id = 0x60000000u;
constexpr std::uint32_t hole_id = asset_id + 0x10000u;
constexpr std::uint32_t flash_id = hole_id + 1u;
struct intent
{
    ::weapon::action action;

    float yaw = 0, pitch = 0;
};

struct casing
{
    vec3 position{}, velocity{};
    quat orientation{};

    float age = 0, spin = 0;
    bool sleeping = false;
    float lifetime = 0;
};

struct hole
{
    mat4 transform{};

    float age = 0, lifetime = 0;
};

struct resource
{
    std::uint32_t skin = 0;
    std::vector<std::uint32_t> meshes;
};

struct state
{
    definition settings;
    ::scene::state asset;
    resource model, casing_mesh;
    std::array<std::uint32_t, std::size_t(motion::count)> clips{};
    ::weapon::state authority, predicted;
    std::vector<draw> draws;
    std::vector<casing> casings;
    std::deque<hole> holes;
    std::deque<std::pair<net::weapon::command, double>> pending;
    std::unordered_set<std::uint32_t> confirmed;
    std::shared_ptr<const ::scene::state> collision_source;
    ::weapon::world collision;
    clock::time_point started{}, previous{};

    mat4 camera_correction = mat4::identity();
    mat4 muzzle_matrix = mat4::identity();

    std::uint32_t skin = 0, camera_joint = UINT32_MAX, muzzle = UINT32_MAX, ejection = UINT32_MAX, slide = UINT32_MAX;
    std::uint32_t clip = 0, sequence = 0, last_fired_sequence = 0, shot_variant = 0;

    double clip_started = 0, flash_until = 0, next_fire = 0;
    float speed = 1, ads = 0, recoil = 0;
    bool loop = false, ready = false, connected = false, presented_aim = false;
};

state data;

std::mutex input_mutex;
std::deque<intent> intents;

bool trigger_held = false, reload_held = false, aim = false, magazine_check_requested = false;

std::uint32_t movement = 0;
clock::time_point next_automatic_fire{};
bool contains(const resource &model, std::uint32_t mesh)
{
    return std::find(model.meshes.begin(), model.meshes.end(), mesh) != model.meshes.end();
}

double seconds()
{
    return std::chrono::duration<double>(clock::now() - data.started).count();
}

mat4 inverse_affine(mat4 m)
{
    const vec3 a{m(0, 0), m(1, 0), m(2, 0)}, b{m(0, 1), m(1, 1), m(2, 1)}, c{m(0, 2), m(1, 2), m(2, 2)};

    const float determinant = dot(a, cross(b, c));

    if (std::abs(determinant) < 1e-10f)
        throw std::runtime_error("Singular weapon socket transform");

    const auto r0 = cross(b, c) / determinant, r1 = cross(c, a) / determinant, r2 = cross(a, b) / determinant;
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
    const vec3 x=normalize(vec3{matrix(0,0),matrix(1,0),matrix(2,0)});
    const vec3 y=normalize(vec3{matrix(0,1),matrix(1,1),matrix(2,1)});
    const vec3 z=normalize(cross(x,y));
    const std::array<vec3,3> axis{x,y,z};
    const auto m=[&](int row,int col){return (&axis[col].x)[row];};
    const float trace=m(0,0)+m(1,1)+m(2,2);
    quat q{};
    if(trace>0) {
        const float k=2*std::sqrt(trace+1);
        q={(m(2,1)-m(1,2))/k,(m(0,2)-m(2,0))/k,
           (m(1,0)-m(0,1))/k,k*.25f};
    } else {
        int a=m(1,1)>m(0,0)?1:0;
        if(m(2,2)>m(a,a))a=2;
        const int b=(a+1)%3,c=(a+2)%3;
        const float k=2*std::sqrt(1+m(a,a)-m(b,b)-m(c,c));
        float v[4]{};
        v[a]=k*.25f;
        v[b]=(m(b,a)+m(a,b))/k;
        v[c]=(m(c,a)+m(a,c))/k;
        v[3]=(m(c,b)-m(b,c))/k;
        q={v[0],v[1],v[2],v[3]};
    }
    return normalize(q);
}

instance_data instance(mat4 model)
{
    return {model, transpose(inverse_affine(model))};
}
::scene::mesh disc(std::uint32_t id, bool flash)
{
    ::scene::mesh mesh;
    mesh.id = id;
    mesh.surface.base_color = flash ? std::array<float, 4>{1, .7f, .18f, 1}
                                    : std::array<float, 4>{.018f, .014f, .012f, 1};
    mesh.surface.roughness = .95f;

    if (flash) {
        mesh.surface.emissive = {8, 5, 1};
    }

    constexpr int points = 16;
    mesh.vertices.push_back({{0, 0, 0}, {0, 0, 1}, .5f, .5f});

    for (int i = 0; i < points; ++i)
    {
        const float a = float(i) * 6.2831853f / points;
        const float radius = flash ? (i % 2 ? .25f : 1.f) : (i % 3 ? 1.f : .86f);

        mesh.vertices.push_back(
            {{std::cos(a) * radius, std::sin(a) * radius, 0}, {0, 0, 1}, 0, 0});
        mesh.indices.insert(mesh.indices.end(),
                            {0u, std::uint32_t(i + 1), std::uint32_t((i + 1) % points + 1)});
    }

    return mesh;
}
::weapon::hit trace_casing(vec3 from, vec3 to)
{
    const vec3 travel = to - from;
    const float distance = length(travel);

    if (distance < 1e-7f) return {};

    return ::weapon::trace(data.collision, from, travel / distance, distance);
}

void add_impact(const ::weapon::hit &impact, const definition &settings)
{
    if (!impact.impacted)
        return;

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

    if (data.holes.size() >= settings.max_impacts)
        data.holes.pop_front();
    data.holes.push_back({m, 0, settings.impact_lifetime});
}

std::uint32_t clip_index(motion name)
{
    return data.skin ? data.clips[std::size_t(name)] : 0u;
}

void play(motion name, double time)
{
    data.clip_started = time;

    if (!data.skin) {
        data.clip = 0;
        data.speed = 1;
        data.loop = false;

        return;
    }

    data.clip = clip_index(name);
    const auto &animation = data.settings.animations[std::size_t(name)];
    data.speed = animation.speed;
    data.loop = animation.loop;
}

bool finished(double time)
{
    return !data.skin || (!data.loop && (time - data.clip_started) * data.speed >= animation::duration(data.skin, data.clip));
}

mat4 socket(std::uint32_t index)
{
    animation::matrix matrix{};

    return animation::joint_transform(data.skin, index, matrix)
               ? mat4{matrix}
               : mat4::identity();
}

void append(::scene::state imported)
{
    auto &dst = data.asset;

    const auto add = [](auto &target, auto &values)
    {
        target.insert(target.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    };

    add(dst.meshes, imported.meshes);
    add(dst.instances, imported.instances);
    add(dst.textures, imported.textures);
    add(dst.skins, imported.skins);
}

resource load_asset(const std::filesystem::path &path, std::uint32_t &next_id)
{
    const auto begun = clock::now();
    auto imported = srv::assets::load(path, next_id, next_id, next_id, next_id);

    if (imported.meshes.empty())
        throw std::runtime_error("Weapon model has no meshes: " + path.string());

    if (imported.skins.size() > 1)
        throw std::runtime_error("Viewmodel meshes must share one skeleton: " + path.string());
    resource result{};

    for (const auto &mesh : imported.meshes)
        result.meshes.push_back(mesh.id);

    if (!imported.skins.empty()) result.skin = imported.skins.front().id;
    next_id += std::uint32_t(std::max({imported.meshes.size(), imported.instances.size(), imported.textures.size(), imported.skins.size()})) + 1u;

    if (next_id >= hole_id)
        throw std::runtime_error("Local model resource ID budget exceeded");
    std::cout << "clt: imported local model " << path.string()
              << " (" << imported.meshes.size() << " meshes, "
              << imported.textures.size() << " textures, "
              << std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - begun).count() << " ms)\n";
    append(std::move(imported));

    return result;
}

void reset_presentation(double now)
{
    camera::set_look_sensitivity(data.settings.input.sensitivity, data.settings.input.ads_sensitivity_multiplier);
    data.skin = data.model.skin;
    data.camera_joint = data.muzzle = data.ejection = data.slide = UINT32_MAX;
    data.camera_correction = data.settings.correction;

    if (data.skin) {
        data.camera_joint = animation::joint(data.skin, data.settings.camera_joint);
        data.muzzle = animation::joint(data.skin, data.settings.muzzle_joint);
        data.ejection = animation::joint(data.skin, data.settings.ejection_joint);
        data.slide = animation::joint(data.skin, data.settings.slide_joint);
        std::vector<animation::matrix> palette;

        if (!animation::sample(data.skin, clip_index(motion::idle), 0, palette))
            throw std::runtime_error("Cannot sample weapon reference pose");
        animation::matrix reference{};

        if (data.camera_joint != UINT32_MAX && animation::joint_transform(data.skin, data.camera_joint, reference))
            data.camera_correction = inverse_affine(mat4{reference}) * data.settings.correction;
    }
    {
        std::lock_guard lock(input_mutex);
        next_automatic_fire = {};
        trigger_held = false;
    }

    data.presented_aim = false;
    data.ads = data.recoil = 0;
    data.flash_until = 0;
    camera::aim(0);
    camera::set_ads_fraction(0);
    play(motion::draw, now);
}
// Keep client reconciliation resets identical for respawn and reconnection.
void reset_reconciliation(double now,bool reconnect)
{
    data.authority=data.predicted=::weapon::make_state(data.settings.rules);
    data.pending.clear();
    data.confirmed.clear();
    data.casings.clear();
    data.holes.clear();
    data.next_fire=data.last_fired_sequence=0;
    if(reconnect)data.sequence=0;
    else {
        data.flash_until=0;
        data.shot_variant=0;
        std::lock_guard lock(input_mutex);
        intents.clear();
        trigger_held=reload_held=aim=magazine_check_requested=false;
        movement=0;
        next_automatic_fire={};
    }
    reset_presentation(now);
}
} // namespace
bindings active_bindings()
{
    std::lock_guard lock(input_mutex);

    return data.settings.rules.id ? data.settings.input : bindings{};
}

void controls(bool trigger, bool ads, bool reload, std::uint32_t buttons)
{
    const auto camera = camera::snapshot();

    std::lock_guard lock(input_mutex);

    const auto now = clock::now();

    if (trigger && intents.size() < 64 && (!trigger_held || (data.settings.input.automatic && now >= next_automatic_fire))) {
        intents.push_back({::weapon::action::fire, camera.yaw, camera.pitch});
        next_automatic_fire = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(data.settings.rules.fire_interval));
    }

    if (reload && !reload_held && intents.size() < 64)
        intents.push_back({::weapon::action::reload, camera.yaw, camera.pitch});
    trigger_held = trigger;

    if (!trigger) next_automatic_fire = {};
    reload_held = reload;
    aim = ads;
    movement = buttons;
}

void register_weapon(definition gun)
{
    if (data.ready || data.settings.rules.id)
        throw std::logic_error("Register one weapon before renderer initialization");
    ::weapon::validate(gun.rules);

    if (gun.model.empty() || !gun.max_casings || gun.max_casings > 192 || !gun.max_impacts || gun.max_impacts > 192 || gun.near_plane <= 0 || gun.far_plane <= gun.near_plane || gun.fov <= 0 || gun.fov >= 3.14159265f ||
        gun.ads_fov <= 0 || gun.ads_fov >= 3.14159265f || !std::isfinite(gun.input.sensitivity) || gun.input.sensitivity <= 0 || gun.input.sensitivity > .1f || !std::isfinite(gun.input.ads_sensitivity_multiplier) ||
        gun.input.ads_sensitivity_multiplier <= 0 || gun.input.ads_sensitivity_multiplier > 1)
        throw std::invalid_argument("Invalid client weapon definition");

    if (!gun.animations[0].name.size() && std::any_of(gun.animations.begin(), gun.animations.end(),
                                                      [](const clip &value)
                                                      { return !value.name.empty(); }))
        throw std::invalid_argument("Animated gun requires idle");

    for (const auto &clip : gun.animations)
        if (!clip.name.empty() && (!std::isfinite(clip.speed) || clip.speed <= 0))
            throw std::invalid_argument("Invalid weapon animation clip");
    data.settings = std::move(gun);
    data.authority = data.predicted = ::weapon::make_state(data.settings.rules);
}

void check_magazine()
{
    std::lock_guard lock(input_mutex);
    magazine_check_requested = true;
}

void initialize()
{
    if (data.ready) return;
    if (!data.settings.rules.id) {
        data.ready = true;

        return;
    }

    data.started = data.previous = clock::now();
    std::uint32_t next_id = asset_id;
    data.model = load_asset(data.settings.model, next_id);

    if (!data.settings.casing_model.empty())
        data.casing_mesh = load_asset(data.settings.casing_model, next_id);

    if (data.casing_mesh.skin || (data.model.skin && data.settings.animations[0].name.empty()) ||
        (!data.model.skin && !data.settings.animations[0].name.empty()))
        throw std::runtime_error("Invalid animated weapon or casing model");

    if (data.model.skin) {
        const auto skin = std::find_if(data.asset.skins.begin(), data.asset.skins.end(), [&](const ::scene::skin &s)
                                       { return s.id == data.model.skin; });
        if (skin == data.asset.skins.end())
            throw std::runtime_error("Weapon skeleton missing");

        for (std::size_t i = 0; i < data.settings.animations.size(); ++i)
        {
            const auto &configured = data.settings.animations[i];

            if (configured.name.empty()) continue;

            const auto found = std::find_if(skin->clips.begin(), skin->clips.end(), [&](const ::scene::clip &entry)
                                            { return entry.name == configured.name; });
            if (found == skin->clips.end())
                throw std::runtime_error("Missing weapon animation: " + configured.name);
            data.clips[i] = std::uint32_t(found - skin->clips.begin());
        }
    }

    animation::synchronize(data.asset);
    data.asset.meshes.push_back(disc(hole_id, false));
    data.asset.meshes.push_back(disc(flash_id, true));
    data.casings.reserve(192);
    reset_presentation(seconds());
    data.ready = true;
}

const ::scene::state &assets() { return data.asset; }
const std::vector<draw> &update(const mat4 &view)
{
    if (!data.settings.rules.id) return data.draws;

    const auto now = seconds();
    static std::uint32_t seen_reset=0;
    const auto reset=connection::get()->reset_generation.load(std::memory_order_acquire);
    if(reset!=seen_reset) {
        seen_reset=reset;
        // Execute on the render/weapon thread; the window, model assets
        // and GPU resources are not recreated.
        reset_reconciliation(now,false);
    }
    const auto frame_time = clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(frame_time - data.previous).count(), 0.f, .05f);
    data.previous = frame_time;
    animation::synchronize(data.asset);

    bool aiming = false, inspect_magazine = false;

    std::uint32_t moving = 0;
    std::deque<intent> inputs;
    {
        std::lock_guard lock(input_mutex);
        aiming = aim;
        moving = movement;
        inputs.swap(intents);
        inspect_magazine = magazine_check_requested;
        magazine_check_requested = false;
    }

    const bool connected = connection::get()->connected.load(std::memory_order_relaxed);

    if (connected != data.connected) {
        data.connected=connected;
        reset_reconciliation(now,true);
    }

    for (const auto &update : connection::take_weapon_updates())
    {
        if (update.weapon_id != data.settings.rules.id || update.active_weapon_id != data.settings.rules.id)
            throw std::runtime_error("Server weapon differs from configured Glock");

        if (update.magazine > data.settings.rules.magazine_capacity || update.reserve > data.settings.rules.initial_reserve)
            continue;

        if (update.outcome == ::weapon::result::fired && net::message::newer(update.sequence, data.last_fired_sequence)) {
            data.last_fired_sequence = update.sequence;

            const auto found = std::find_if(data.pending.begin(), data.pending.end(), [&](const auto &item)
                                            { return item.first.sequence == update.sequence; });
            data.next_fire = (found == data.pending.end() ? now : found->second) + data.settings.rules.fire_interval;
        }

        std::erase_if(data.pending, [&](const auto &item)
                      { return !net::message::newer(item.first.sequence, update.sequence); });
        data.authority.magazine = update.magazine;
        data.authority.reserve = update.reserve;
        data.authority.reload = update.reload;
        data.authority.sequence = update.sequence;
        data.authority.next_fire = data.next_fire;

        if (update.reload != ::weapon::reload_phase::none) {
            data.authority.reload_started = now - update.reload_elapsed;
            data.authority.reload_finishes = data.authority.reload_started + update.reload_duration;
            data.authority.refill_at = data.authority.reload_started + update.refill_time;
            data.authority.refilled = update.reload_elapsed >= update.refill_time;
        }

        data.predicted = data.authority;

        for (const auto &item : data.pending)
            if (item.first.kind != ::weapon::action::equip)
                ::weapon::apply(data.predicted, item.first.sequence, item.first.kind, item.second, data.settings.rules);
        ::weapon::advance(data.predicted, now, data.settings.rules);

        if (data.predicted.reload != ::weapon::reload_phase::none) {
            const auto m = data.predicted.reload == ::weapon::reload_phase::empty
                               ? motion::reload_empty
                               : motion::reload;

            if (data.clip != clip_index(m)) {
                play(m, data.predicted.reload_started);
                data.presented_aim = false;
            }
        }
        else if (data.clip == clip_index(motion::reload) || data.clip == clip_index(motion::reload_empty)) {
            play(motion::idle, now);
            data.presented_aim = false;
        }
    }

    const auto scene = connection::snapshot();

    for (const auto &shot : connection::take_weapon_shots())
        if (shot.state.weapon_id == data.settings.rules.id && data.confirmed.insert(shot.state.sequence).second)
            add_impact(shot.impact, data.settings);

    if (data.confirmed.size() > 1024) data.confirmed.clear();
    ::weapon::advance(data.predicted, now, data.settings.rules);

    if (inspect_magazine) {
        std::cout << "magazine: " << data.predicted.magazine << "/"
                  << data.settings.rules.magazine_capacity
                  << " | reserve: " << data.predicted.reserve;

        if (data.predicted.reload != ::weapon::reload_phase::none)
            std::cout << " | reloading: " << std::max(0., data.predicted.reload_finishes - now) << "s";
        std::cout << std::endl;
    }

    bool fired = false;

    for (const auto &event : inputs)
    {
        if (!connected) continue;
        if (event.action == ::weapon::action::fire && (!data.predicted.magazine || data.predicted.reload != ::weapon::reload_phase::none))
            continue;

        if (event.action == ::weapon::action::reload && (data.predicted.magazine >= data.settings.rules.magazine_capacity || !data.predicted.reserve || data.predicted.reload != ::weapon::reload_phase::none))
            continue;
        net::weapon::command command{};
        command.sequence = data.sequence + 1;

        if (!command.sequence) ++command.sequence;
        command.weapon_id = data.settings.rules.id;
        command.kind = event.action;
        command.yaw = event.yaw;
        command.pitch = event.pitch;

        if (!connection::send_weapon(command)) continue;
        data.sequence = command.sequence;
        data.pending.push_back({command, now});

        const auto outcome = ::weapon::apply(data.predicted, command.sequence, command.kind, now, data.settings.rules);

        if (outcome == ::weapon::result::fired) {
            const auto variant = ++data.shot_variant % 3;
            play(motion(int(aiming ? motion::ads_fire1 : motion::fire1) + int(variant)), now);
            data.presented_aim = aiming;
            data.flash_until = now + data.settings.flash_duration;
            data.recoil = std::min(data.recoil + data.settings.recoil, data.settings.recoil * 3);
            camera::kick(aiming ? data.settings.ads_kick : data.settings.hip_kick);
            fired = true;
        }
        else if (outcome == ::weapon::result::reload_started) {
            play(data.predicted.reload == ::weapon::reload_phase::empty ? motion::reload_empty : motion::reload, now);
            data.presented_aim = false;
        }
    }

    const auto idle_motion = aiming ? (moving ? motion::ads_walk : motion::ads_idle)
                                    : (moving ? motion::walk : motion::idle);

    const auto idle = clip_index(idle_motion);
    const auto idle_transition = [&]
    {
        if (aiming != data.presented_aim) {
            play(aiming ? motion::ads_in : motion::ads_out, now);
            data.presented_aim = aiming;
        }
        else
            play(idle_motion, now);
    };

    if (finished(now) || (data.loop && (data.clip != idle || aiming != data.presented_aim)))
        idle_transition();
    data.ads += (float(aiming) - data.ads) * (1 - std::exp(-data.settings.ads_speed * dt));
    data.recoil *= std::exp(-data.settings.recoil_decay * dt);
    camera::aim(data.ads * data.settings.ads_zoom);
    camera::set_ads_fraction(data.ads);
    std::vector<animation::matrix> palette;

    if (data.skin && !animation::sample(data.skin, data.clip, float(now - data.clip_started) * data.speed, palette, UINT32_MAX, 0, UINT32_MAX, data.loop))
        throw std::runtime_error("Cannot sample weapon animation");

    if (data.skin && data.slide != UINT32_MAX && !data.predicted.magazine && data.predicted.reload == ::weapon::reload_phase::none) {
        const bool firing_clip = data.clip == clip_index(motion::fire1) || data.clip == clip_index(motion::fire2) || data.clip == clip_index(motion::fire3) || data.clip == clip_index(motion::ads_fire1) ||
                                 data.clip == clip_index(motion::ads_fire2) || data.clip == clip_index(motion::ads_fire3);

        if (!firing_clip || finished(now)) {
            const auto move = inverse_affine(data.camera_correction) * vec4{0, 0, data.settings.slide_offset, 0};

            animation::offset_joint(data.skin, data.slide,
                                    {move.x, move.y, move.z}, palette);
        }
    }

    const auto camera_world = inverse_affine(view);
    const auto local = translate({0, 0, data.recoil}) * rotation(angle_axis(data.recoil, {1, 0, 0})) * data.camera_correction;
    const auto model = camera_world * local;
    data.muzzle_matrix = model * (data.muzzle == UINT32_MAX ? translate(data.settings.muzzle_offset) : socket(data.muzzle));

    if (fired && !data.settings.casing_model.empty()) {
        const auto port = model * (data.ejection == UINT32_MAX ? translate(data.settings.ejection_offset) : socket(data.ejection));
        const auto p = port * vec4{0, 0, 0, 1};
        const auto impulse = data.settings.casing_impulse;
        const auto v = camera_world * vec4{impulse.x, impulse.y, impulse.z, 0};

        if (data.casings.size() >= data.settings.max_casings)
            data.casings.erase(data.casings.begin());
        data.casings.push_back({{p.x, p.y, p.z}, {v.x, v.y, v.z}, orientation(port), 0, data.settings.casing_spin, false, data.settings.casing_lifetime});
    }

    if (scene != data.collision_source) {
        bool appended = data.collision_source && scene && ::scene::geometry_prefix_unchanged(*data.collision_source, *scene);

        if (scene) {
            if (!appended) data.collision = {};

            const auto first = appended ? data.collision_source->instances.size() : 0;
            ::weapon::append(data.collision, scene->meshes, std::span<const ::scene::instance>(scene->instances).subspan(first));
        }
        else
            data.collision = {};
        data.collision_source = scene;
    }

    for (auto &c : data.casings)
    {
        c.age += dt;

        if (c.sleeping) continue;

        const int steps = std::max(1, int(std::ceil(dt / .008f)));
        const float step_dt = dt / steps;

        for (int step = 0; step < steps; ++step)
        {
            c.velocity.y -= 9.81f * step_dt;

            const auto displacement = c.velocity * step_dt;
            const float distance = length(displacement);

            if (distance < 1e-7f) continue;

            const auto hit = trace_casing(c.position, c.position + displacement);

            if (hit.impacted && length(hit.position - c.position) <= distance + .004f) {
                c.position = hit.position + hit.normal * .004f;

                const float inward = dot(c.velocity, hit.normal);

                if (inward < 0)
                    c.velocity = (c.velocity - hit.normal * (1.35f * inward)) * .65f;
                c.spin *= .65f;

                if (length(c.velocity) < .16f && hit.normal.y > .5f) c.sleeping = true;
            }
            else
                c.position = c.position + displacement;
            c.orientation = angle_axis(c.spin * step_dt, {.3f, .7f, .2f}) * c.orientation;
        }
    }

    std::erase_if(data.casings, [](const casing &c)
                  { return c.age > c.lifetime || c.position.y < -100; });
    for (auto &h : data.holes)
        h.age += dt;
    std::erase_if(data.holes, [](const hole &h)
                  { return h.age > h.lifetime; });
    data.draws.clear();

    if (!connected) return data.draws;

    for (const auto &mesh : data.asset.meshes)
    {
        draw item{};
        item.mesh_id = mesh.id;

        if (mesh.id == hole_id) {
            for (const auto &h : data.holes)
                item.instances.push_back(instance(h.transform));
        }
        else if (mesh.id == flash_id) {
            item.viewmodel = true;

            if (now < data.flash_until) {
                const auto p = data.muzzle_matrix * vec4{0, 0, 0, 1};
                auto flash = camera_world;
                flash(0, 3) = p.x;
                flash(1, 3) = p.y;
                flash(2, 3) = p.z;
                item.instances.push_back(instance(flash * rotation(angle_axis(float(data.shot_variant) * 2.4f, {0, 0, 1})) * scale(data.settings.flash_scale)));
            }
        }
        else if (contains(data.model, mesh.id)) {
            item.viewmodel = true;
            item.palette = palette;

            for (const auto &node : data.asset.instances)
                if (node.mesh_id == mesh.id)
                    item.instances.push_back(instance(model * trs(node.pose.position, node.pose.rotation, node.pose.scale)));
        }
        else if (contains(data.casing_mesh, mesh.id)) {
            for (const auto &c : data.casings)
                item.instances.push_back(instance(trs(c.position, c.orientation, {1, 1, 1})));
        }

        if (!item.instances.empty()) data.draws.push_back(std::move(item));
    }

    return data.draws;
}

mat4 view_projection(float aspect, const mat4 &view)
{
    if (!data.settings.rules.id) return view;

    return perspective(data.settings.fov + (data.settings.ads_fov - data.settings.fov) * data.ads, aspect, data.settings.near_plane, data.settings.far_plane) * view;
}

void shutdown()
{
    data = state{};
    std::lock_guard lock(input_mutex);
    intents.clear();
    trigger_held = reload_held = aim = magazine_check_requested = false;
    movement = 0;
    next_automatic_fire = {};
}

} // namespace clt::weapon
