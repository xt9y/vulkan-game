#pragma once
#include "../../server/net/scene_stream.hpp"
#include "../../server/game/secure_config.hpp"
#include "../../server/net/animation.hpp"
#include "../../server/net/editor_light.hpp"
#include "../../server/net/editor_cube.hpp"
#include "../../server/net/movement.hpp"
#include "../../server/net/weapon.hpp"
#include "../../server/player.hpp"
#include "../../server/scene.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <network/endpoint.hpp>
#include <network/secure.hpp>
#include <network/session.hpp>
#include <network/socket.hpp>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
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
    std::atomic<bool> scene_complete{false};
    std::atomic<bool> respawn_requested{false},respawn_pending{false};
    std::atomic<std::uint32_t> respawn_input{0},reset_generation{0};
    std::mutex animation_mutex;
    std::unordered_map<std::uint32_t, net::animation::resource> animations;
    std::mutex scene_mutex;
    std::shared_ptr<const ::scene::state> scene;
    std::vector<::scene::state> pending_scene;
    std::chrono::steady_clock::time_point last_publish{};
    std::map<std::uint32_t, net::editor_cube::resource> cube_states;
    bool cubes_dirty = false;
    std::array<std::uint32_t, 64> light_versions{};
    std::array<std::optional<net::editor_light::resource>, 64> light_pending{};
    std::mutex error_mutex;
    std::exception_ptr worker_error;
    std::mutex player_mutex;
    std::deque<::player::input> outgoing;
    std::optional<net::movement::snapshot> authority;
    std::optional<::player::parameters_t> server_parameters;
    std::mutex peers_mutex;
    net::movement::peer_snapshot peers;
    std::chrono::steady_clock::time_point peers_received{};
    std::mutex editor_mutex;
    std::deque<net::editor_light::resource> editor_outgoing;
    std::uint32_t editor_sequence = 0;
    std::deque<net::editor_cube::resource> cube_outgoing;
    std::uint32_t cube_sequence = 0;
    std::mutex weapon_mutex;
    std::deque<net::weapon::command> weapon_outgoing;
    std::deque<net::weapon::snapshot> weapon_updates;
    std::deque<net::weapon::shot> weapon_shots;
    std::uint32_t weapon_ack = 0;
    std::uint32_t weapon_seen_id = 0, weapon_seen_sequence = 0, weapon_seen_tick = 0;
    std::thread worker;

    bool initialized = false;
};

std::shared_ptr<const ::scene::state> snapshot();
std::optional<net::animation::resource> animation(std::uint32_t instance_id);

using clock = std::chrono::steady_clock;

using namespace std::chrono_literals;
namespace
{
void run(const std::string &host, std::uint16_t port);
}

state *get()
{
    static state data;

    return &data;
}

void reset_scene(state &data)
{
    std::lock_guard lock(data.scene_mutex);
    data.scene.reset();
    data.pending_scene.clear();
    data.last_publish = {};
    data.cube_states.clear();
    data.cubes_dirty = false;
    data.light_versions.fill(0);
    data.light_pending.fill(std::nullopt);
    {
        std::lock_guard peers_lock(data.peers_mutex);
        data.peers = {};
        data.peers_received = {};
    }
}

void reset_queues(state &data, bool reset_parameters)
{
    {
        std::lock_guard lock(data.player_mutex);
        data.outgoing.clear();
        data.authority.reset();

        if (reset_parameters) data.server_parameters.reset();
    }
    {
        std::lock_guard lock(data.editor_mutex);
        data.editor_outgoing.clear();
        data.cube_outgoing.clear();
        data.cube_sequence = 0;
        data.editor_sequence = 0;
    }

    reset_scene(data);
}

std::shared_ptr<const scene::state> snapshot()
{
    auto &data = *get();
    std::lock_guard lock(data.scene_mutex);
    std::size_t count = data.scene ? data.scene->lights.size() : 0;

    for (const auto &batch : data.pending_scene)
        count += batch.lights.size();

    bool pending_light = false;

    for (std::size_t i = 0; i < data.light_pending.size() && i <= count; ++i)
        if (data.light_pending[i]) {
            pending_light = true;

            break;
        }

    if (data.pending_scene.empty() && (!data.scene || (!pending_light && !data.cubes_dirty)))
        return data.scene;

    const auto now = clock::now();

    if (data.scene && !data.scene_complete.load(std::memory_order_relaxed) && now - data.last_publish < 16ms) return data.scene;

    auto combined = data.scene
                        ? std::make_shared<::scene::state>(*data.scene)
                        : std::make_shared<::scene::state>();
    // New asset batches change reflected UV layouts and the atlas. Property
    // changes and lighting revisions reuse the original packed textures.
    for(const auto& patch:data.pending_scene)
        if(!patch.meshes.empty() || !patch.textures.empty()) {
            ++combined->atlas_revision;
            break;
        }

    const auto append = [&](auto field)
    {
        auto &output = (*combined).*field;
        std::size_t total = output.size();

        for (const auto &patch : data.pending_scene)
            total += (patch.*field).size();
        output.reserve(total);

        for (auto &patch : data.pending_scene)
        {
            auto &source = patch.*field;
            output.insert(output.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
        }
    };

    append(&::scene::state::skins);
    append(&::scene::state::textures);
    append(&::scene::state::meshes);
    std::unordered_map<std::uint32_t, std::size_t> instance_slots;
    instance_slots.reserve(combined->instances.size());
    for (std::size_t i = 0; i < combined->instances.size(); ++i)
        instance_slots.emplace(combined->instances[i].id, i);
    for (auto &patch : data.pending_scene)
        for (auto &instance : patch.instances) {
            const auto [slot, inserted] = instance_slots.emplace(instance.id, combined->instances.size());
            if (inserted) combined->instances.push_back(std::move(instance));
            else combined->instances[slot->second] = std::move(instance);
        }
    append(&::scene::state::collision_boxes);
    append(&::scene::state::lights);

    const auto light_value = [](const ::scene::point_light &l)
    {
        return std::tuple{l.position.x, l.position.y, l.position.z, l.radius, l.color.x, l.color.y, l.color.z, l.strength, l.enabled};
    };

    bool changed = false;

    for (std::size_t i = 0; i < data.light_pending.size(); ++i)
    {
        auto &pending = data.light_pending[i];

        if (!pending || i > combined->lights.size()) continue;
        if (i == combined->lights.size()) {
            combined->lights.push_back(pending->light);
            changed = true;
        }
        else if (light_value(combined->lights[i]) != light_value(pending->light)) {
            combined->lights[i] = pending->light;
            changed = true;
        }

        pending.reset();
    }

    // Retain authoritative values even after publication: a later stream batch
    // may still contain the cube's pre-edit pose or a deleted cube.
    for (const auto &[id, update] : data.cube_states)
        changed = net::editor_cube::apply_state(*combined, update) || changed;
    data.cubes_dirty = false;

    if (data.pending_scene.empty() && !changed) return data.scene;
    data.scene = std::move(combined);
    data.pending_scene.clear();
    data.last_publish = now;

    return data.scene;
}

std::optional<net::animation::resource> animation(std::uint32_t instance_id)
{
    auto &data = *get();
    std::lock_guard<std::mutex> lock(data.animation_mutex);

    const auto it = data.animations.find(instance_id);

    if (it == data.animations.end()) return std::nullopt;

    return it->second;
}

// One in-order editor command queue for both cube and light packets.
// The in-flight front is never overwritten, and coalescing keeps its sequence.
template<class Command>
bool queue_editor(Command command,std::deque<Command>& outgoing,
                  std::uint32_t& sequence)
{
    if(outgoing.size()>1 && command.type==net::editor_light::kind::edit) {
        auto& last=outgoing.back();
        if(last.type==command.type && last.index==command.index &&
           last.revision==command.revision) {
            command.sequence=last.sequence;
            last=command;
            return true;
        }
    }
    if(outgoing.size()>=64)return false;
    command.sequence=++sequence;
    if(!command.sequence)command.sequence=++sequence;
    if(command.type==net::editor_light::kind::create)command.index=UINT32_MAX;
    outgoing.push_back(command);
    return true;
}
bool send_editor_light(net::editor_light::resource command)
{
    if((command.type!=net::editor_light::kind::create &&
        command.type!=net::editor_light::kind::edit &&
        command.type!=net::editor_light::kind::remove) ||
       !net::editor_light::valid(command.light) ||
       !get()->connected.load(std::memory_order_relaxed))return false;
    auto& data=*get();
    std::lock_guard lock(data.editor_mutex);
    return queue_editor(command,data.editor_outgoing,data.editor_sequence);
}
bool send_editor_cube(net::editor_cube::resource command)
{
    using namespace net::editor_cube;
    if((command.type!=kind::create && command.type!=kind::edit &&
        command.type!=kind::remove) || !valid(command.cube) ||
       !get()->connected.load(std::memory_order_relaxed))return false;
    auto& data=*get();
    std::lock_guard lock(data.editor_mutex);
    return queue_editor(command,data.cube_outgoing,data.cube_sequence);
}

bool send_weapon(net::weapon::command command)
{
    auto &data = *get();
    std::lock_guard<std::mutex> lock(data.weapon_mutex);

    if (data.weapon_outgoing.size() >= 64) return false;
    data.weapon_outgoing.push_back(command);

    return true;
}

std::deque<net::weapon::snapshot> take_weapon_updates()
{
    auto &data = *get();
    std::lock_guard lock(data.weapon_mutex);

    return std::exchange(data.weapon_updates, {});
}

std::deque<net::weapon::shot> take_weapon_shots()
{
    auto &data = *get();
    std::lock_guard lock(data.weapon_mutex);

    return std::exchange(data.weapon_shots, {});
}

void check_error()
{
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(get()->error_mutex);
        error = get()->worker_error;
    }

    if (error)
        std::rethrow_exception(error);
}

void request_respawn(std::uint32_t last_input)
{
    auto& data=*get();
    if(!data.connected.load(std::memory_order_relaxed))return;
    {
        std::lock_guard lock(data.player_mutex);
        data.outgoing.clear();
        data.authority.reset();
    }
    {
        std::lock_guard lock(data.weapon_mutex);
        data.weapon_outgoing.clear();
        data.weapon_updates.clear();
        data.weapon_shots.clear();
    }
    data.respawn_input.store(last_input,std::memory_order_relaxed);
    data.respawn_pending.store(true,std::memory_order_release);
    data.respawn_requested.store(true,std::memory_order_release);
    data.reset_generation.fetch_add(1,std::memory_order_relaxed);
}
bool respawning()
{
    return get()->respawn_pending.load(std::memory_order_acquire);
}
void send_input(const ::player::input &input)
{
    auto &data = *get();
    std::lock_guard<std::mutex> lock(data.player_mutex);

    if (data.outgoing.size() >= 128)
        data.outgoing.pop_front();
    data.outgoing.push_back(input);
}

std::optional<net::movement::snapshot> take_authority()
{
    auto &data = *get();
    std::lock_guard<std::mutex> lock(data.player_mutex);

    return std::exchange(data.authority, std::nullopt);
}

std::optional<::player::parameters_t> movement_parameters()
{
    auto &data = *get();
    std::lock_guard<std::mutex> lock(data.player_mutex);

    return data.server_parameters;
}

std::vector<net::movement::peer> remote_players()
{
    auto &data = *get();
    std::lock_guard lock(data.peers_mutex);

    if (clock::now() - data.peers_received > 500ms) return {};

    return data.peers.players;
}

void init()
{
    if (get()->initialized || get()->worker.joinable())
        throw std::runtime_error("clt::connection already initialized");
    reset_queues(*get(), true);
    {
        std::lock_guard<std::mutex> lock(get()->error_mutex);
        get()->worker_error = {};
    }

    get()->stop.store(false, std::memory_order_relaxed);
    get()->connected.store(false, std::memory_order_relaxed);
    get()->server_tick.store(0, std::memory_order_relaxed);
    get()->scene_complete.store(false, std::memory_order_relaxed);
    get()->respawn_pending.store(false,std::memory_order_relaxed);
    get()->respawn_requested.store(false,std::memory_order_relaxed);
    get()->worker = std::thread(run, get()->host, get()->port);
    get()->initialized = true;
}

void shutdown() noexcept
{
    get()->stop.store(true, std::memory_order_relaxed);

    if (get()->worker.joinable())
        get()->worker.join();
    reset_queues(*get(), false);
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
void run(const std::string &host, std::uint16_t port)
{
    auto &data = *get();
    try
    {
        const auto server = net::endpoint::resolve(host, port);
        data.socket = net::secure::create(net::secure::role::client, game::secure_config::load(net::secure::role::client, host, port), 0, server.address.ss_family == AF_INET6 ? "::" : "0.0.0.0");
        net::secure::connect(data.socket, server);
        net::session::settings transport_settings;
        transport_settings.channel.max_message = net::scene_stream::max_scene_bytes + 20;
        transport_settings.channel.max_queued = 2 * (net::scene_stream::max_scene_bytes + 20);
        transport_settings.channel.window = 512;
        transport_settings.max_delivered_bytes = 2 * (net::scene_stream::max_scene_bytes + 20);
        transport_settings.max_peers = 1;
        transport_settings.allow_unknown_peers = false;
        net::session transport(transport_settings);
        transport.open(server);
        std::uint32_t token = std::random_device{}() | 1u;
        std::uint32_t sequence = 0;
        std::uint32_t outstanding = 0;
        std::uint32_t last_accepted = 0;

        bool waiting = false;

        std::uint32_t last_player_tick = 0;
        std::uint32_t reset_message_sequence=0;
        std::uint32_t respawn_server_tick=0;
        clock::time_point next_reset_send{};
        std::deque<::player::input> unacknowledged;
        std::uint32_t scene_batch = 0;
        clock::time_point stream_started{};
        std::size_t transferred_bytes = 0;

        bool scene_ready = false;
        auto next_send = clock::now();
        auto next_scene_send = next_send;
        auto next_move_send = next_send;
        auto next_weapon_send = next_send;
        auto next_editor_send = next_send;
        auto next_cube_send = next_send;
        std::uint32_t cube_retry_sequence = 0, cube_retry_attempts = 0;

        std::uint32_t editor_retry_sequence = 0;
        std::uint32_t editor_retry_attempts = 0;

        auto last_received = next_send;
        auto retry = std::chrono::milliseconds(net::limits::retry_ms);

        std::array<std::uint8_t, 1473> buffer{};

        auto clear_scene = [&]
        {
            {
                std::lock_guard<std::mutex> lock(data.weapon_mutex);
                data.weapon_outgoing.clear();
                data.weapon_updates.clear();
                data.weapon_shots.clear();
                data.weapon_ack = 0;
                data.weapon_seen_id = data.weapon_seen_sequence = data.weapon_seen_tick = 0;
            }
            {
                std::lock_guard<std::mutex> lock(data.editor_mutex);
                data.editor_outgoing.clear();
                data.cube_outgoing.clear();
                cube_retry_sequence = cube_retry_attempts = 0;
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
            reset_scene(data);
            {
                std::lock_guard<std::mutex> animation_lock(data.animation_mutex);
                data.animations.clear();
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
            if(data.respawn_requested.exchange(false,std::memory_order_acq_rel)) {
                reset_message_sequence=++sequence;
                next_reset_send=now;
                unacknowledged.clear();
            }
            if(data.respawn_pending.load(std::memory_order_acquire) &&
               reset_message_sequence && now>=next_reset_send &&
               data.connected.load(std::memory_order_relaxed)) {
                const auto packet=net::message::encode({
                    net::message::type::respawn,reset_message_sequence,token,
                    data.respawn_input.load(std::memory_order_relaxed)});
                net::secure::send(data.socket,server,packet.data(),packet.size());
                next_reset_send=now+100ms;
            }

            for (int budget = 0; budget < 512; ++budget)
            {
                net::endpoint::resource peer;

                const auto size = net::secure::receive(data.socket, peer, buffer.data(), buffer.size());

                if (!size)
                    break;

                if (!net::endpoint::equal(peer, server)) continue;
                // A decoded packet is consumed even if authentication fails.
                // Only the configured endpoint reaches this dispatcher.
                const auto dispatch = [&](auto decode, auto accept) {
                    const auto packet = decode(buffer.data(), *size);
                    if (!packet) return false;
                    const auto packet_token = [&] {
                        if constexpr (requires { packet->token; }) return packet->token;
                        else return packet->state.token;
                    }();
                    if (packet_token == token && data.connected.load(std::memory_order_relaxed)) accept(*packet);
                    return true;
                };
                const auto accept_editor = [&](const auto& update, auto& outgoing, auto merge) {
                    if (update.type != net::editor_light::kind::state && update.type != net::editor_light::kind::ack) return;
                    if (update.type == net::editor_light::kind::ack) {
                        std::lock_guard lock(data.editor_mutex);
                        if (!outgoing.empty() && outgoing.front().sequence == update.sequence) outgoing.pop_front();
                    }
                    {
                        std::lock_guard lock(data.scene_mutex);
                        merge(update);
                    }
                    last_received = now;
                };
                if (dispatch(net::editor_cube::decode, [&](const auto& update) {
                    accept_editor(update, data.cube_outgoing, [&](const auto& value) {
                        const auto found = data.cube_states.find(value.index);
                        if ((found == data.cube_states.end() && data.cube_states.size() < net::editor_cube::max_cubes) ||
                            (found != data.cube_states.end() && net::message::newer(value.revision, found->second.revision))) {
                            data.cube_states[value.index] = value;
                            data.cubes_dirty = true;
                        }
                    });
                }) || dispatch(net::editor_light::decode, [&](const auto& update) {
                    accept_editor(update, data.editor_outgoing, [&](const auto& value) {
                        auto& revision = data.light_versions[value.index];
                        if (net::message::newer(value.revision, revision)) {
                            revision = value.revision;
                            data.light_pending[value.index] = value;
                        }
                    });
                })) continue;

                auto accept_weapon = [&](const net::weapon::snapshot &update)
                {
                    if(data.respawn_pending.load(std::memory_order_acquire) ||
                       (respawn_server_tick &&
                        !net::message::newer(update.tick,respawn_server_tick)))
                        return false;
                    std::lock_guard<std::mutex> lock(data.weapon_mutex);

                    if (data.weapon_seen_id == update.weapon_id &&
                        (net::message::newer(data.weapon_seen_sequence, update.sequence) ||
                         (update.sequence == data.weapon_seen_sequence &&
                          net::message::newer(data.weapon_seen_tick, update.tick))))
                        return false;
                    data.weapon_seen_id = update.weapon_id;
                    data.weapon_seen_sequence = update.sequence;
                    data.weapon_seen_tick = update.tick;

                    if (!net::message::newer(data.weapon_ack, update.sequence)) {
                        data.weapon_ack = update.sequence;
                    }

                    while (!data.weapon_outgoing.empty() && !net::message::newer(data.weapon_outgoing.front().sequence, data.weapon_ack))
                        data.weapon_outgoing.pop_front();

                    if (data.weapon_updates.size() >= 64)
                        data.weapon_updates.pop_front();
                    data.weapon_updates.push_back(update);
                    last_received = now;

                    return true;
                };

                if (dispatch(net::weapon::decode_shot, [&](const auto& shot) {
                    if (data.respawn_pending.load(std::memory_order_acquire) ||
                        (respawn_server_tick && !net::message::newer(shot.state.tick, respawn_server_tick))) return;
                    accept_weapon(shot.state);
                    std::lock_guard lock(data.weapon_mutex);
                    if (data.weapon_shots.size() >= 64) data.weapon_shots.pop_front();
                    data.weapon_shots.push_back(shot);
                }) || dispatch(net::weapon::decode_snapshot, accept_weapon) ||
                dispatch(net::animation::decode, [&](const auto& value) {
                    std::lock_guard lock(data.animation_mutex);
                    const auto old = data.animations.find(value.instance_id);
                    if (old == data.animations.end() || net::message::newer(value.server_tick, old->second.server_tick))
                        data.animations[value.instance_id] = value;
                    last_received = now;
                }) || dispatch(net::movement::decode_parameters, [&](const auto& settings) {
                    std::lock_guard lock(data.player_mutex);
                    data.server_parameters = settings.values;
                    last_received = now;
                }) || dispatch(net::movement::decode_peers, [&](const auto& peers) {
                    std::lock_guard lock(data.peers_mutex);
                    if (data.peers_received == clock::time_point{} || net::message::newer(peers.tick, data.peers.tick)) {
                        data.peers = peers;
                        data.peers_received = now;
                    }
                    last_received = now;
                }) || dispatch(net::movement::decode_snapshot, [&](const auto& update) {
                    last_received = now;
                    if (!net::message::newer(update.tick, last_player_tick)) return;
                    last_player_tick = update.tick;
                    while (!unacknowledged.empty() && !net::message::newer(unacknowledged.front().sequence, update.sequence))
                        unacknowledged.pop_front();
                    std::lock_guard lock(data.player_mutex);
                    data.authority = update;
                })) continue;

                const auto message = net::message::decode(buffer.data(), *size);
                if(message && message->type==net::message::type::respawned &&
                   message->token==token &&
                   message->sequence==reset_message_sequence &&
                   data.respawn_pending.load(std::memory_order_acquire)) {
                    last_player_tick=respawn_server_tick=message->value;
                    unacknowledged.clear();
                    {
                        std::lock_guard lock(data.weapon_mutex);
                        data.weapon_updates.clear();
                        data.weapon_shots.clear();
                        data.weapon_seen_id=data.weapon_seen_sequence=data.weapon_seen_tick=0;
                    }
                    {
                        std::lock_guard lock(data.player_mutex);
                        data.authority.reset();
                        data.outgoing.clear();
                    }
                    data.respawn_pending.store(false,std::memory_order_release);
                    last_received=now;
                    next_move_send=now;
                    continue;
                }

                if (transport.accept(peer, buffer.data(), *size, now))
                    continue;

                if (!message || message->token != token || !net::message::newer(message->sequence, last_accepted) || net::message::newer(message->sequence, outstanding) || !waiting)
                    continue;

                const bool is_connected = data.connected.load(std::memory_order_relaxed);

                if ((!is_connected && message->type != net::message::type::welcome) || (is_connected && message->type != net::message::type::state))
                    continue;
                last_accepted = message->sequence;
                data.server_tick.store(message->value, std::memory_order_relaxed);
                data.connected.store(true, std::memory_order_relaxed);
                last_received = now;
                next_send = now + std::chrono::milliseconds(net::limits::heartbeat_ms);
                waiting = false;
                retry = std::chrono::milliseconds(net::limits::retry_ms);
            }

            transport.pump([&](const net::endpoint::resource &peer, const void *bytes, std::size_t size)
                           { return net::secure::send(data.socket, peer, bytes, size); }, 64 * 1024, now);
            net::delivery completed;

            while (transport.pop(completed))
            {
                net::scene_stream::batch_payload batch;

                if (scene_ready || !net::scene_stream::decode_batch(completed.payload, batch) || batch.token != token || batch.batch != scene_batch)
                    continue;

                if (stream_started == clock::time_point{})
                    stream_started = clock::now();
                ::scene::state received;

                if (!net::scene_stream::deserialize(batch.scene, received)) {
                    clear_scene();
                    next_scene_send = now;

                    continue;
                }

                transferred_bytes += batch.scene.size();
                {
                    std::lock_guard<std::mutex> lock(data.scene_mutex);
                    std::cout << "clt: received scene batch " << scene_batch
                              << " (" << received.meshes.size() << " meshes, "
                              << received.instances.size() << " instances, "
                              << received.textures.size() << " textures, "
                              << received.skins.size() << " skins)\n";
                    data.pending_scene.push_back(std::move(received));
                }

                scene_ready = batch.final;
                data.scene_complete.store(scene_ready, std::memory_order_relaxed);
                ++scene_batch;
                next_scene_send = now;

                if (scene_ready && stream_started != clock::time_point{}) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - stream_started).count();
                    std::cout << "clt: scene transferred in " << elapsed
                              << " ms (" << transferred_bytes / 1024 << " KiB)\n";
                }
            }

            if (data.connected.load(std::memory_order_relaxed) && now - last_received > std::chrono::milliseconds(net::limits::connection_timeout_ms)) {
                data.connected.store(false, std::memory_order_relaxed);
                data.respawn_pending.store(false,std::memory_order_relaxed);
                data.respawn_requested.store(false,std::memory_order_relaxed);
                reset_message_sequence=0;
                respawn_server_tick=0;
                clear_scene();
                net::secure::connect(data.socket, server);

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

            if (now >= next_send) {
                send(data.connected.load(std::memory_order_relaxed) ? net::message::type::ping : net::message::type::hello);

                if (waiting)
                    retry = std::min(retry * 2, std::chrono::milliseconds(net::limits::retry_max_ms));
                waiting = true;
                next_send = now + retry;
            }

            if (data.connected.load(std::memory_order_relaxed) && !scene_ready && now >= next_scene_send) {
                const auto packet = net::message::encode({net::message::type::scene_request, ++sequence, token, (scene_batch << 16)});
                net::secure::send(data.socket, server, packet.data(), packet.size());

                if (stream_started == clock::time_point{})
                    stream_started = now;
                next_scene_send = now + std::chrono::milliseconds(net::limits::scene_request_ms);
            }

            if (data.connected.load(std::memory_order_relaxed) && now >= next_weapon_send) {
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

            if (data.connected.load(std::memory_order_relaxed) &&
                !data.respawn_pending.load(std::memory_order_acquire)) {
                for (const auto &input : inputs)
                    unacknowledged.push_back(input);

                if (!unacknowledged.empty() && now >= next_move_send) {
                    auto transmit = [&](const ::player::input &input)
                    {
                        const auto packet = net::movement::encode(token, input);
                        net::secure::send(data.socket, server, packet.data(), packet.size());
                    };

                    for (std::size_t i = 0; i < std::min<std::size_t>(8, unacknowledged.size()); ++i)
                        transmit(unacknowledged[i]);

                    if (unacknowledged.size() > 8)
                        transmit(unacknowledged.back());
                    next_move_send = now + std::chrono::milliseconds(net::limits::movement_ms);
                }
            }
            else {
                unacknowledged.clear();
            }

            // Shared reliable resend for both authoritative editor packets.
            const auto retry_editor = [&](auto &queue, auto &next,
                                          auto &retry_sequence, auto &attempts,
                                          auto encode, const char *label) {
                if (!data.connected.load(std::memory_order_relaxed) || now < next) return;
                using entry = std::remove_cvref_t<decltype(queue.front())>;
                std::optional<entry> pending;
                {
                    std::lock_guard lock(data.editor_mutex);
                    if (!queue.empty()) pending = queue.front();
                }
                if (pending) {
                    if (pending->sequence != retry_sequence) {
                        retry_sequence = pending->sequence;
                        attempts = 0;
                    }
                    if (attempts++ < 100u) {
                        pending->token = token;
                        const auto packet = encode(*pending);
                        net::secure::send(data.socket, server, packet.data(), packet.size());
                    } else {
                        std::lock_guard lock(data.editor_mutex);
                        if (!queue.empty() && queue.front().sequence == pending->sequence)
                            queue.pop_front();
                        std::clog << "clt: editor " << label
                                  << " request not acknowledged (check server editor permissions)\n";
                        retry_sequence = attempts = 0;
                    }
                } else {
                    retry_sequence = attempts = 0;
                }
                next = now + 50ms;
            };
            retry_editor(data.cube_outgoing, next_cube_send,
                         cube_retry_sequence, cube_retry_attempts,
                         net::editor_cube::encode, "cube");
            retry_editor(data.editor_outgoing, next_editor_send,
                         editor_retry_sequence, editor_retry_attempts,
                         net::editor_light::encode, "light");

            auto wake = data.connected.load(std::memory_order_relaxed) && !scene_ready
                            ? std::min(next_send, next_scene_send)
                            : next_send;

            if (!unacknowledged.empty())
                wake = std::min(wake, next_move_send);

            const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(wake - clock::now());
            net::secure::wait_readable(data.socket, std::clamp(delay, 1ms, data.connected.load(std::memory_order_relaxed) ? 5ms : 50ms));
        }

        if (data.connected.load(std::memory_order_relaxed))
            send(net::message::type::goodbye);
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
