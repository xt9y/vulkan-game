#pragma once
#include "../net/editor_light.hpp"
#include "../net/editor_cube.hpp"
#include "../net/movement.hpp"
#include "../net/scene_stream.hpp"
#include "../player.hpp"
#include "../scene.hpp"
#include "../weapon.hpp"
#include "session.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <jobs/pool.hpp>
#include <map>
#include <memory>
#include <network/endpoint.hpp>
#include <network/secure.hpp>
#include <network/session.hpp>
#include <spatial/index.hpp>
#include <thread>
#include <vector>

namespace srv::application
{
struct model_request
{
    std::filesystem::path path;
    ::scene::transform pose;
    std::uint32_t animation;
};

struct state
{
    net::secure::resource socket;
    std::unique_ptr<net::session> scene_transport;
    scene::state scene;
    std::vector<std::uint32_t> editor_light_versions;
    std::uint32_t editor_light_revision = 1;
    std::map<std::uint32_t, net::editor_cube::resource> editor_cubes;
    std::uint32_t editor_cube_revision = 1;
    std::uint32_t next_cube_id = 0x80000000u;
    std::vector<model_request> models;
    std::vector<::scene::instance> mermaid_template;

    bool scene_configured = false;

    std::shared_ptr<scene::state> importing;
    enum import_slot
    {
        texture_slot,
        skin_slot,
        mesh_slot,
        instance_slot
    };

    std::array<std::size_t, 4> import_cursor{};
    std::size_t animation_cursor = 0;
    player::collision_world colliders;
    ::weapon::world weapon_world;
    ::weapon::definition weapon{};
    spatial::index collision_index;
    struct batch
    {
        std::vector<std::uint8_t> bytes;

        bool final = false;
    };

    std::vector<batch> scene_batches;
    std::uint32_t tick = 0;

    bool initialized = false;
};

state *get();
void set_scene(::scene::state scene);
void load_model(std::filesystem::path path, ::scene::transform pose, std::uint32_t animation);
void add_weapon(::weapon::definition definition);
void init();
void begin_stream();
void append_scene(::scene::state patch, bool final = false);
int run();
void shutdown() noexcept;

using clock = std::chrono::steady_clock;

using namespace std::chrono_literals;
namespace
{
template <class T>
bool queue_input(std::deque<T> &queue, const T &item, std::uint32_t processed, std::size_t capacity)
{
    if (!net::message::newer(item.sequence, processed) || item.sequence - processed > 256) return false;
    if (std::any_of(queue.begin(), queue.end(), [&](const auto &value)
                    { return value.sequence == item.sequence; })) return true;
    auto expected = processed + 1;
    if (!expected) ++expected;
    if (queue.size() >= capacity) {
        if (item.sequence != expected) return false;
        queue.pop_back();
    }

    queue.insert(std::find_if(queue.begin(), queue.end(), [&](const auto &value)
                              { return net::message::newer(value.sequence, item.sequence); }),
                 item);
    return true;
}

jobs::pool &generation_jobs()
{
    static jobs::pool worker;

    return worker;
}

void receive_messages();
void send_editor_lights();
void send_editor_cubes();
void handle_editor_cube(const net::endpoint::resource &, const net::editor_cube::resource &);
void handle_editor_light(const net::endpoint::resource &, const net::editor_light::resource &);
void send_scene_batches();
void send_animation_states();
void handle(const net::endpoint::resource &peer, const net::message::resource &message);
void reply(const net::endpoint::resource &peer, net::message::type type, const net::message::resource &request);
void tick_players();
void tick_weapons();
void handle_weapon(const net::endpoint::resource &peer, const net::weapon::command &command);
net::weapon::snapshot weapon_snapshot(const session::resource &client, double now);
} // namespace
state *get()
{
    static state data;

    return &data;
}

void set_scene(::scene::state scene)
{
    if (get()->initialized) throw std::runtime_error("Cannot replace a running scene");
    get()->scene = std::move(scene);
    get()->editor_light_versions.assign(get()->scene.lights.size(), 1u);
    get()->editor_light_revision = 1;
    get()->editor_cubes.clear();
    get()->mermaid_template.clear();
    get()->editor_cube_revision = 1;
    get()->next_cube_id = 0x80000000u;
    for (const auto &instance : get()->scene.instances) {
        if (instance.mesh_id != ::scene::ground_mesh_id &&
            instance.mesh_id != ::scene::unit_cube_mesh_id &&
            instance.mesh_id != ::scene::unit_sphere_mesh_id &&
            !instance.editor_group)continue;
        net::editor_cube::resource update{};
        update.index = instance.id;
        update.cube = instance;
        update.sequence = update.revision = 1;
        get()->editor_cubes.emplace(instance.id, update);
    }
    get()->scene_configured = true;
}

void load_model(std::filesystem::path path, ::scene::transform pose, std::uint32_t animation)
{
    if (get()->initialized) throw std::runtime_error("Register model imports before init");
    if (path.empty() || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("Model file not found: " + path.string());
    get()->models.push_back({std::move(path), pose, animation});
}

void add_weapon(::weapon::definition definition)
{
    if (get()->initialized)
        throw std::runtime_error("Register weapons before init");

    if (get()->weapon.id)
        throw std::logic_error("Only one weapon belongs to this game");
    ::weapon::validate(definition);
    get()->weapon = std::move(definition);
}

void init()
{
    if (get()->initialized)
        throw std::runtime_error("srv::application already initialized");

    if (!get()->scene_configured)
        throw std::runtime_error("The game must provide a scene with set_scene()");
    get()->socket = net::secure::create(net::secure::role::server, game::secure_config::load(net::secure::role::server), game::options::get()->port, game::options::get()->bind_host);
    net::session::settings transport_settings;
    transport_settings.channel.max_message = net::scene_stream::max_scene_bytes + 20;
    transport_settings.channel.max_queued = 2 * (net::scene_stream::max_scene_bytes + 20);
    transport_settings.channel.window = 512;
    transport_settings.max_peers = net::limits::max_clients;
    transport_settings.allow_unknown_peers = false;
    get()->scene_transport = std::make_unique<net::session>(transport_settings);
    session::init();
    get()->colliders = player::build_colliders(get()->scene);
    get()->collision_index = player::build_index(get()->colliders);
    ::weapon::append(get()->weapon_world, get()->scene.meshes, get()->scene.instances);
    generation_jobs().start(1, 64);
    get()->scene_batches = {{net::scene_stream::serialize(get()->scene), true}};
    get()->tick = 0;
    get()->initialized = true;

    auto requests = std::move(get()->models);

    if (!requests.empty()) {
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
                    // Runtime editor IDs occupy the upper half of the space.
                    // Imported prefab IDs must not jump into that range.
                    if(instance.id<0x80000000u)
                        instance_id=std::max(instance_id,instance.id+1);
                for (const auto& texture : get()->scene.textures)
                    texture_id = std::max(texture_id, texture.id + 1);
                for (const auto& skin : get()->scene.skins)
                    skin_id = std::max(skin_id, skin.id + 1);
                for (const auto& request : requests) {
                    const auto import_started = clock::now();
auto imported = assets::load(request.path, mesh_id, instance_id, texture_id, skin_id);
                    if (imported.meshes.empty())
                        throw std::runtime_error("Model has no renderable meshes: " + request.path.string());
                    // The mermaid is one selectable prefab even when its
                    // source GLB contains several distinct materials/meshes.
                    if(request.path.filename()=="mermaid.glb" &&
                       !imported.instances.empty()) {
                        const auto root=imported.instances.front().id;
                        for(auto& inst:imported.instances)inst.editor_group=root;
                    }
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
                    auto& template_instances=get()->mermaid_template;
                    template_instances.clear();
                    for(const auto& inst:imported->instances)
                        if(inst.editor_group)template_instances.push_back(inst);
                    get()->importing = std::move(imported);
                    get()->import_cursor={};
                }; }))
            throw std::runtime_error("Could not queue model imports");
    }
}

void begin_stream()
{
    auto &data = *get();
    data.scene_batches = {{net::scene_stream::serialize(data.scene), false}};
}

void append_scene(::scene::state patch, bool final)
{
    auto &data = *get();

    if (!data.initialized || data.scene_batches.empty() || data.scene_batches.back().final)
        throw std::runtime_error("Scene stream not accepting batches");

    if (data.scene_batches.size() >= 65535)
        throw std::runtime_error("Too many scene batches");

    auto bytes = net::scene_stream::serialize(patch);
    const auto append = [](auto &dst, auto &src)
    {
        dst.insert(dst.end(), std::make_move_iterator(src.begin()),
                   std::make_move_iterator(src.end()));
    };

    append(data.scene.skins, patch.skins);
    append(data.scene.textures, patch.textures);
    append(data.scene.meshes, patch.meshes);
    ::weapon::append(data.weapon_world, data.scene.meshes, patch.instances);

    auto added_colliders = player::build_colliders(data.scene.meshes, patch.instances);

    for(const auto& instance:patch.instances)
        if(instance.editor_group) {
            net::editor_cube::resource update{};
            update.index=instance.id;
            update.cube=instance;
            update.sequence=update.revision=1;
            data.editor_cubes.emplace(instance.id,update);
        }
    append(data.scene.instances, patch.instances);
    append(data.scene.collision_boxes, patch.collision_boxes);
    data.editor_light_versions.insert(data.editor_light_versions.end(),
                                      patch.lights.size(), data.editor_light_revision);
    append(data.scene.lights, patch.lights);
    data.collision_index.append(player::collision_bounds(added_colliders));
    append(data.colliders, added_colliders);
    data.scene_batches.push_back({std::move(bytes), final});
}

int run()
{
    if (!get()->initialized)
        throw std::runtime_error("srv::application is not initialized");

    const auto started = clock::now();
    auto next = started;
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / net::limits::tick_hz);
    while (!game::application::get()->stop.load(std::memory_order_relaxed))
    {
        const auto now = clock::now();
        if (game::options::get()->duration > 0 && std::chrono::duration<double>(now - started).count() >= game::options::get()->duration)
            break;
        ++get()->tick;
        session::expire(now);
        receive_messages();
        tick_players();
        tick_weapons();
        send_editor_lights();
        send_editor_cubes();
        send_animation_states();
        if (get()->importing) {
            auto &imported = *get()->importing;
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
                if (get()->import_cursor[state::texture_slot] < imported.textures.size()) {
                    auto &texture = imported.textures[get()->import_cursor[state::texture_slot]];
                    size = 20 + (texture.encoded.empty() ? texture.pixels.size() : texture.encoded.size());
                    if (!fits(size)) break;
                    patch.textures.push_back(std::move(texture));
                    ++get()->import_cursor[state::texture_slot];
                }
                else if (get()->import_cursor[state::skin_slot] < imported.skins.size()) {
                    auto &skin = imported.skins[get()->import_cursor[state::skin_slot]];
                    size = 16 + skin.skeleton.size() + skin.inverse_bind.size() * 64;
                    for (const auto &clip : skin.clips)
                        size += 8 + clip.name.size() + clip.bytes.size();
                    if (!fits(size)) break;
                    patch.skins.push_back(std::move(skin));
                    ++get()->import_cursor[state::skin_slot];
                }
                else if (get()->import_cursor[state::mesh_slot] < imported.meshes.size()) {
                    auto &mesh = imported.meshes[get()->import_cursor[state::mesh_slot]];
                    size = 96 + mesh.vertices.size() * (mesh.skin_id ? 64 : 32) + mesh.indices.size() * 4;
                    if (!fits(size)) break;
                    patch.meshes.push_back(std::move(mesh));
                    ++get()->import_cursor[state::mesh_slot];
                }
                else if (get()->import_cursor[state::instance_slot] < imported.instances.size()) {
                    size = 108; // Serialized instance including editor group.
                    if (!fits(size)) break;
                    patch.instances.push_back(std::move(imported.instances[get()->import_cursor[state::instance_slot]++]));
                }
                else
                    break;
                used += size;
                if (used >= target_bytes) break;
            }
            const bool final = get()->import_cursor[state::texture_slot] == imported.textures.size() && get()->import_cursor[state::skin_slot] == imported.skins.size() && get()->import_cursor[state::mesh_slot] == imported.meshes.size() && get()->import_cursor[state::instance_slot] == imported.instances.size();
            if (!patch.textures.empty() || !patch.skins.empty() || !patch.meshes.empty() || !patch.instances.empty())
                append_scene(std::move(patch), final);
            if (final) {
                std::size_t bytes = 0;
                for (const auto &batch : get()->scene_batches)
                    bytes += batch.bytes.size();
                std::cout << "srv: model stream ready: " << get()->scene_batches.size()
                          << " batches, " << bytes / 1024 << " KiB on the wire\n";
                get()->importing.reset();
            }
        }
        send_scene_batches();
        get()->scene_transport->pump([&](const net::endpoint::resource &peer, const void *bytes, std::size_t size)
                                     { return net::secure::send(get()->socket, peer, bytes, size); }, 512 * 1200);
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
void shutdown() noexcept
{
    get()->scene_transport.reset();
    net::secure::destroy(get()->socket);
    session::shutdown();
    generation_jobs().stop();
    get()->scene_batches.clear();
    get()->colliders.clear();
    get()->weapon_world = {};
    get()->weapon = {};
    get()->scene = {};
    get()->editor_cubes.clear();
    get()->mermaid_template.clear();
    get()->editor_light_versions.clear();
    get()->editor_light_revision = 1;
    get()->models.clear();
    get()->scene_configured = false;
    get()->importing.reset();
    get()->import_cursor = {};
    get()->animation_cursor = 0;
    get()->tick = 0;
    get()->initialized = false;
}
namespace
{
bool editor_local(const net::endpoint::resource &peer)
{
    if (peer.address.ss_family == AF_INET) {
        const auto *addr = reinterpret_cast<const sockaddr_in *>(&peer.address);
        return reinterpret_cast<const unsigned char *>(&addr->sin_addr)[0] == 127;
    }
    if (peer.address.ss_family == AF_INET6) {
        const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&peer.address);
        return IN6_IS_ADDR_LOOPBACK(&addr->sin6_addr) != 0;
    }
    return false;
}
void handle_editor_light(const net::endpoint::resource &peer, const net::editor_light::resource &command)
{
    if (!editor_local(peer)) return;
    for (auto &client : session::get()->clients)
    {
        if (!client.active || client.token != command.token || !net::endpoint::equal(client.endpoint, peer))
            continue;
        const bool duplicate = command.sequence == client.editor_light_sequence;
        if (!duplicate && (!net::message::newer(command.sequence, client.editor_light_sequence) || (client.editor_light_sequence && command.sequence - client.editor_light_sequence > 256u)))
            return;
        if (!duplicate) {
            std::uint32_t index = command.index;
            if (command.type == net::editor_light::kind::create) {
                const auto vacant = std::find_if(get()->scene.lights.begin(), get()->scene.lights.end(), [](const ::scene::point_light &light)
                                                 { return !light.enabled; });
                if (vacant != get()->scene.lights.end()) {
                    index = static_cast<std::uint32_t>(vacant - get()->scene.lights.begin());
                    *vacant = command.light;
                    vacant->enabled = true;
                    get()->editor_light_versions[index] = ++get()->editor_light_revision;
                }
                else {
                    if (get()->scene.lights.size() >= 64) return;
                    index = static_cast<std::uint32_t>(get()->scene.lights.size());
                    get()->scene.lights.push_back(command.light);
                    get()->scene.lights.back().enabled = true;
                    get()->editor_light_versions.push_back(++get()->editor_light_revision);
                }
            }
            else if (command.type == net::editor_light::kind::edit) {
                if (index >= get()->scene.lights.size() || !get()->scene.lights[index].enabled || command.revision > 7) return;
                auto &light = get()->scene.lights[index];
                *net::editor_light::property_fields(light)[command.revision] = *net::editor_light::property_fields(command.light)[command.revision];
                get()->editor_light_versions[index] = ++get()->editor_light_revision;
            }
            else if (command.type == net::editor_light::kind::remove) {
                if (index >= get()->scene.lights.size() || !get()->scene.lights[index].enabled) return;
                auto &light = get()->scene.lights[index];
                light.enabled = false;
                light.strength = 0.0f; // No direct/indirect contribution.
                get()->editor_light_versions[index] = ++get()->editor_light_revision;
            }
            else
                return;
            client.editor_light_sequence = command.sequence;
            client.editor_light_index = index;
            for (const auto &recipient : session::get()->clients)
            {
                if (!recipient.active) continue;
                net::editor_light::resource changed{};
                changed.type = net::editor_light::kind::state;
                changed.token = recipient.token;
                changed.index = index;
                changed.sequence = get()->editor_light_versions[index];
                changed.revision = changed.sequence;
                changed.light = get()->scene.lights[index];
                const auto bytes = net::editor_light::encode(changed);
                net::secure::send(get()->socket, recipient.endpoint, bytes.data(), bytes.size());
            }
            std::clog << "srv: editor light " << index
                      << (command.type == net::editor_light::kind::create ? " placed" : command.type == net::editor_light::kind::remove ? " deleted"
                                                                                                                                        : " updated")
                      << "\n";
        }
        if (client.editor_light_index >= get()->scene.lights.size())
            return;
        net::editor_light::resource ack{};
        ack.type = net::editor_light::kind::ack;
        ack.token = client.token;
        ack.sequence = client.editor_light_sequence;
        ack.index = client.editor_light_index;
        ack.revision = get()->editor_light_versions[ack.index];
        ack.light = get()->scene.lights[ack.index];
        const auto bytes = net::editor_light::encode(ack);
        net::secure::send(get()->socket, peer, bytes.data(), bytes.size());
        client.last_seen = clock::now();
        return;
    }
}
void handle_editor_cube(const net::endpoint::resource &peer, const net::editor_cube::resource &command)
{
    using namespace net::editor_cube;
    if (!editor_local(peer) ||
        (command.type != kind::create && command.type != kind::edit &&
         command.type != kind::remove)) return;
    auto &data = *get();
    for (auto &client : session::get()->clients) {
        if (!client.active || client.token != command.token || !net::endpoint::equal(client.endpoint, peer)) continue;
        const bool duplicate = command.sequence == client.editor_cube_sequence;
        if (!duplicate && (!net::message::newer(command.sequence, client.editor_cube_sequence) ||
            (client.editor_cube_sequence && command.sequence - client.editor_cube_sequence > 256u))) return;
        if (!duplicate) {
            resource changed{};
            const bool insert_mermaid=command.type==kind::create &&
                command.cube.mesh_id==::scene::mermaid_insert_mesh_id;
            if(command.type==kind::create) {
                const std::size_t needed=insert_mermaid ?
                    data.mermaid_template.size():1u;
                if(needed==0 || data.editor_cubes.size()+needed>max_cubes ||
                   data.next_cube_id>UINT32_MAX-needed ||
                   (!insert_mermaid &&
                    command.cube.mesh_id!=::scene::unit_cube_mesh_id &&
                    command.cube.mesh_id!=::scene::unit_sphere_mesh_id) ||
                   (!insert_mermaid &&
                    std::none_of(data.scene.meshes.begin(),data.scene.meshes.end(),
                        [&](const auto& m){return m.id==command.cube.mesh_id;})))
                    return;
                changed=command;
                changed.index=data.next_cube_id++;
                changed.cube.id=changed.index;
                // Only a validated imported prefab may carry group metadata.
                changed.cube.editor_group=0;
                if(insert_mermaid) {
                    // All prefab primitives remain separate for their own
                    // GLB textures, while one root owns the editor controls.
                    changed.cube=data.mermaid_template.front();
                    changed.cube.id=changed.index;
                    changed.cube.editor_group=changed.index;
                    changed.cube.pose=command.cube.pose;
                }
                changed.enabled=true;
            }
            else {
                const auto found=data.editor_cubes.find(command.index);
                if(found==data.editor_cubes.end() || !found->second.enabled)return;
                changed=found->second;
                if(command.type==kind::remove)changed.enabled=false;
                else if(!set_property(changed.cube,command.revision,command.value))
                    return;
            }
            const auto previous=data.editor_cubes.find(changed.index);
            const bool physical=previous==data.editor_cubes.end() ||
                previous->second.enabled!=changed.enabled ||
                !::scene::same_pose(previous->second.cube,changed.cube);
            changed.type=kind::state;
            changed.sequence=changed.revision=++data.editor_cube_revision;
            if(!changed.sequence)
                changed.sequence=changed.revision=++data.editor_cube_revision;
            changed.value=0;
            std::vector<resource> updates{changed};
            if(insert_mermaid) {
                const auto& original=data.mermaid_template.front();
                for(std::size_t i=1;i<data.mermaid_template.size();++i) {
                    auto copy=changed;
                    copy.index=data.next_cube_id++;
                    copy.cube=data.mermaid_template[i];
                    copy.cube.id=copy.index;
                    copy.cube.editor_group=changed.index;
                    copy.cube.pose.position=changed.cube.pose.position+
                        (copy.cube.pose.position-original.pose.position);
                    copy.cube.pose.scale=changed.cube.pose.scale;
                    copy.cube.pose.rotation=changed.cube.pose.rotation;
                    copy.cube.rotation_reference=changed.cube.rotation_reference;
                    updates.push_back(copy);
                }
            }
            else if(changed.cube.editor_group) {
                // One edit applies to every GLB primitive in the same
                // imported model without combining their PBR materials.
                const auto before=previous->second.cube.pose;
                for(const auto& [id,stored]:data.editor_cubes) {
                    if(id==changed.index ||
                       stored.cube.editor_group!=changed.cube.editor_group)
                        continue;
                    auto part=stored;
                    part.enabled=changed.enabled;
                    part.type=kind::state;
                    part.revision=part.sequence=changed.revision;
                    part.cube.pose.position=stored.cube.pose.position+
                        (changed.cube.pose.position-before.position);
                    part.cube.pose.scale=changed.cube.pose.scale;
                    part.cube.pose.rotation=changed.cube.pose.rotation;
                    part.cube.rotation_reference=changed.cube.rotation_reference;
                    updates.push_back(part);
                }
            }
            for(auto& update:updates) {
                data.editor_cubes[update.index]=update;
                apply_state(data.scene,update);
            }
            if(physical) {
                data.colliders=player::build_colliders(data.scene);
                data.collision_index=player::build_index(data.colliders);
                data.weapon_world={};
                ::weapon::append(data.weapon_world,data.scene.meshes,
                                 data.scene.instances);
            }
            client.editor_cube_sequence=command.sequence;
            client.editor_cube_index=changed.index;
            for(const auto& recipient:session::get()->clients) {
                if(!recipient.active)continue;
                for(auto update:updates) {
                    update.token=recipient.token;
                    const auto bytes=encode(update);
                    net::secure::send(data.socket,recipient.endpoint,
                                      bytes.data(),bytes.size());
                }
            }
        }
        const auto found = data.editor_cubes.find(client.editor_cube_index);
        if (found == data.editor_cubes.end()) return;
        auto ack = found->second;
        ack.type = kind::ack;
        ack.token = client.token;
        ack.sequence = client.editor_cube_sequence;
        const auto bytes = encode(ack);
        net::secure::send(data.socket, peer, bytes.data(), bytes.size());
        client.last_seen = clock::now();
        return;
    }
}

void send_editor_cubes()
{
    // Periodic state repair covers lost broadcasts, late joins and deletions.
    // Spread the bounded table over ticks rather than sending a large UDP burst.
    constexpr std::size_t per_tick = 16;
    if (get()->editor_cubes.empty()) return;
    constexpr std::size_t cycle = (net::editor_cube::max_cubes + per_tick - 1) / per_tick;
    const std::size_t start = (get()->tick % cycle) * per_tick;
    if (start >= get()->editor_cubes.size()) return;
    auto it = get()->editor_cubes.begin();
    std::advance(it, start);
    for (std::size_t i = 0; i < std::min(per_tick, get()->editor_cubes.size() - start); ++i, ++it) {
        for (const auto &client : session::get()->clients) {
            if (!client.active) continue;
            auto update = it->second;
            update.token = client.token;
            const auto bytes = net::editor_cube::encode(update);
            net::secure::send(get()->socket, client.endpoint, bytes.data(), bytes.size());
        }
    }
}

void send_editor_lights()
{
    if (get()->tick % 60u != 0u) return;
    for (const auto &client : session::get()->clients)
    {
        if (!client.active) continue;
        for (std::size_t i = 0; i < get()->scene.lights.size(); ++i)
        {
            net::editor_light::resource update{};
            update.type = net::editor_light::kind::state;
            update.token = client.token;
            update.index = static_cast<std::uint32_t>(i);
            update.sequence = i < get()->editor_light_versions.size() ? get()->editor_light_versions[i] : 1;
            update.revision = update.sequence;
            update.light = get()->scene.lights[i];
            const auto bytes = net::editor_light::encode(update);
            net::secure::send(get()->socket, client.endpoint, bytes.data(), bytes.size());
        }
    }
}
void receive_messages()
{
    std::array<std::uint8_t, 1473> buffer{};
    const auto deadline = clock::now() + std::chrono::milliseconds(net::limits::receive_ms);
    for (std::uint32_t budget = 0;
         budget < net::limits::receive_packets && clock::now() < deadline;
         ++budget)
    {
        net::endpoint::resource peer;
        const auto size = net::secure::receive(get()->socket, peer, buffer.data(), buffer.size());
        if (!size)
            break;
        if (get()->scene_transport->accept(peer, buffer.data(), *size, clock::now()))
            continue;
        if (const auto command = net::editor_cube::decode(buffer.data(), *size)) {
            handle_editor_cube(peer, *command);
            continue;
        }
        if (const auto editor_command = net::editor_light::decode(buffer.data(), *size)) {
            if (editor_command->type == net::editor_light::kind::create || editor_command->type == net::editor_light::kind::edit || editor_command->type == net::editor_light::kind::remove)
                handle_editor_light(peer, *editor_command);
            continue;
        }
        if (const auto command = net::weapon::decode_command(buffer.data(), *size)) {
            handle_weapon(peer, *command);
            continue;
        }
        if (const auto command = net::movement::decode_input(buffer.data(), *size)) {
            for (auto &client : session::get()->clients)
            {
                if (!client.active || client.token != command->token || !net::endpoint::equal(client.endpoint, peer))
                    continue;
                if (!queue_input(client.inputs, command->input, client.processed_input, 128)) break;
                client.last_seen = clock::now();
                break;
            }
            continue;
        }
        if (const auto message = net::message::decode(buffer.data(), *size))
            handle(peer, *message);
    }
}
net::weapon::snapshot weapon_snapshot(const session::resource &client, double now)
{
    const auto &value = client.weapon;
    net::weapon::snapshot snapshot;
    snapshot.token = client.token;
    snapshot.sequence = client.weapon_sequence;
    snapshot.weapon_id = get()->weapon.id;
    snapshot.active_weapon_id = get()->weapon.id;
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
void handle_weapon(const net::endpoint::resource &peer, const net::weapon::command &command)
{
    for (auto &client : session::get()->clients)
    {
        if (!client.active || client.token != command.token || !net::endpoint::equal(client.endpoint, peer)) continue;
        const auto now = clock::now();
        client.last_seen = now;
        if (!net::message::newer(command.sequence, client.weapon_sequence)) {
            if (client.last_shot && client.last_shot->state.sequence == command.sequence && client.last_shot->state.weapon_id == command.weapon_id) {
                const auto bytes = net::weapon::encode_shot(*client.last_shot);
                net::secure::send(get()->socket, client.endpoint, bytes.data(), bytes.size());
            }
            return;
        }
        auto &queue = client.weapon_commands;
        if (!queue_input(queue, command, client.weapon_sequence, 64)) return;
        const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        while (!queue.empty())
        {
            auto expected = client.weapon_sequence + 1;
            if (!expected) ++expected;
            if (queue.front().sequence != expected) break;
            const auto cmd = queue.front();
            queue.pop_front();
            client.weapon_sequence = cmd.sequence;
            client.weapon.last_result = ::weapon::result::none;
            const auto *definition = get()->weapon.id == cmd.weapon_id
                                         ? &get()->weapon
                                         : nullptr;
            if (!definition) continue;
            const auto result = ::weapon::apply(
                client.weapon, cmd.sequence, cmd.kind, seconds, *definition);
            if (cmd.kind == ::weapon::action::fire) {
                const auto impact = result == ::weapon::result::fired
                                        ? ::weapon::trace(get()->weapon_world, client.avatar.position,
                                                          ::weapon::direction(cmd.yaw, cmd.pitch), definition->range)
                                        : ::weapon::hit{};
                net::weapon::shot shot{weapon_snapshot(client, seconds), impact};
                client.last_shot = shot;
                if (client.shots.size() >= 32) client.shots.pop_front();
                client.shots.push_back({shot, 3});
            }
        }
        return;
    }
}
void tick_weapons()
{
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    for (auto &client : session::get()->clients)
    {
        if (!client.active) continue;
        if (const auto *definition = get()->weapon.id ? &get()->weapon : nullptr) {
            ::weapon::advance(client.weapon, now, *definition);
            const auto state = weapon_snapshot(client, now);
            const auto bytes = net::weapon::encode_snapshot(state);
            net::secure::send(get()->socket, client.endpoint, bytes.data(), bytes.size());
        }
        for (auto &pending : client.shots)
        {
            const auto bytes = net::weapon::encode_shot(pending.value);
            net::secure::send(get()->socket, client.endpoint, bytes.data(), bytes.size());
            --pending.repeats;
        }
        std::erase_if(client.shots, [](const auto &shot)
                      { return shot.repeats == 0; });
    }
}
void handle(const net::endpoint::resource &peer, const net::message::resource &message)
{
    session::resource *existing = nullptr;
    session::resource *available = nullptr;
    for (auto &client : session::get()->clients)
    {
        if (client.active && net::endpoint::equal(client.endpoint, peer))
            existing = &client;
        if (!client.active && !available)
            available = &client;
    }
    if (message.type == net::message::type::hello) {
        auto *client = existing ? existing : available;
        if (!client || message.token == 0)
            return;
        if (!client->active || client->token != message.token) {
            get()->scene_transport->close(peer);
            *client = session::resource{};
            client->endpoint = peer;
            client->token = message.token;
            client->active = true;
            client->avatar.position = player::parameters().spawn;
            if (get()->weapon.id)
                client->weapon = ::weapon::make_state(get()->weapon);
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
    }
    else if (message.type == net::message::type::ping) {
        if (existing->has_sequence && !net::message::newer(message.sequence, existing->last_sequence))
            return;
        existing->last_sequence = message.sequence;
        existing->has_sequence = true;
        existing->last_seen = clock::now();
        reply(peer, net::message::type::state, message);
    }
    else if (message.type == net::message::type::scene_request) {
        existing->last_seen = clock::now();
        existing->scene_request = message.value;
        existing->scene_pending = true;
    }
}
void send_animation_states()
{
    if (get()->tick % net::limits::animation_period || get()->scene.instances.empty()) return;
    const auto &world = get()->scene;
    const auto count = std::min<std::size_t>(net::limits::animations_per_tick, world.instances.size());
    const auto start = get()->animation_cursor % world.instances.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &instance = world.instances[(start + i) % world.instances.size()];
        const auto mesh = std::find_if(world.meshes.begin(), world.meshes.end(),
                                       [&](const ::scene::mesh &value)
                                       { return value.id == instance.mesh_id; });
        if (mesh == world.meshes.end() || !mesh->skin_id) continue;
        for (const auto &client : session::get()->clients)
        {
            if (!client.active) continue;
            const auto packet = net::animation::encode({client.token, get()->tick, instance.id, instance.animation, instance.animation_tick, instance.overlay_animation, instance.overlay_root, instance.overlay_weight});
            net::secure::send(get()->socket, client.endpoint, packet.data(), packet.size());
        }
    }
    get()->animation_cursor = (start + count) % world.instances.size();
}
void send_scene_batches()
{
    const auto now = clock::now();
    for (auto &client : session::get()->clients)
    {
        if (!client.active || !client.scene_pending) continue;
        client.scene_pending = false;
        const auto batch_id = client.scene_request >> 16;
        if (batch_id >= get()->scene_batches.size()) continue;
        if (client.queued_scene_batch == batch_id && (get()->scene_transport->pending(client.endpoint) > 0 || now - client.queued_scene_at < 2s))
            continue;
        const auto &batch = get()->scene_batches[batch_id];
        auto payload = net::scene_stream::encode_batch(batch.bytes, client.token, batch_id, batch.final);
        if (get()->scene_transport->send(client.endpoint, std::move(payload))) {
            client.queued_scene_batch = batch_id;
            client.queued_scene_at = now;
        }
        else
            client.scene_pending = true;
    }
}
void reply(const net::endpoint::resource &peer, net::message::type type, const net::message::resource &request)
{
    const auto data = net::message::encode({type, request.sequence, request.token, get()->tick});
    net::secure::send(get()->socket, peer, data.data(), data.size());
    const auto settings = net::movement::encode_parameters(request.token, player::parameters());
    net::secure::send(get()->socket, peer, settings.data(), settings.size());
}
void tick_players()
{
    for (auto &client : session::get()->clients)
    {
        if (!client.active)
            continue;
        for (std::uint32_t budget = 0; budget < net::limits::inputs_per_tick && !client.inputs.empty(); ++budget)
        {
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
    if (get()->tick % 6u != 0u) return;
    const auto &clients = session::get()->clients;
    for (std::size_t i = 0; i < clients.size(); ++i)
    {
        if (!clients[i].active) continue;
        net::movement::peer_snapshot peers{clients[i].token, get()->tick, {}};
        for (std::size_t j = 0; j < clients.size(); ++j)
            if (i != j && clients[j].active)
                peers.players.push_back({std::uint32_t(j), clients[j].avatar.position});
        const auto bytes = net::movement::encode_peers(peers);
        net::secure::send(get()->socket, clients[i].endpoint, bytes.data(), bytes.size());
    }
}
} // namespace

} // namespace srv::application
