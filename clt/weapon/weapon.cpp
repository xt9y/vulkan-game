#include "weapon.hpp"

#include "clt/network/connection.hpp"
#include "clt/player/prediction.hpp"
#include "clt/view/camera.hpp"
#include "srv/assets/model.hpp"

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
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            aiming = aim;
            moving = movement;
            input.swap(intents);
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
