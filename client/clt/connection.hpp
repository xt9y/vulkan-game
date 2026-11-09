#pragma once
#include "../../server/game/secure_config.hpp"
#include <network/endpoint.hpp>
#include <network/session.hpp>
#include <network/socket.hpp>
#include <cstring>
#include <iterator>
#include <random>
#include <iostream>
#include <algorithm>
#include <utility>
#include "../../server/net/animation.hpp"
#include "../../server/net/editor_light.hpp"
#include "../../server/net/movement.hpp"
#include "../../server/net/weapon.hpp"
#include "../../server/player.hpp"
#include "../../server/scene.hpp"
#include <network/secure.hpp>
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

namespace clt::connection {
    struct state {
        std::string host;
        std::uint16_t port = 0;
        net::secure::resource socket;
        std::atomic<bool> stop{false};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> server_tick{0};
        std::atomic<bool> scene_complete{false};
        std::mutex animation_mutex;
        std::unordered_map<std::uint32_t, net::animation::resource> animations;
        std::mutex scene_mutex;
        std::shared_ptr<const ::scene::state> scene;
        std::vector<::scene::state> pending_scene;
        std::chrono::steady_clock::time_point last_publish{};
        std::array<std::uint32_t,64> light_versions{};
        std::array<std::optional<net::editor_light::resource>,64> light_pending{};
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
        std::uint32_t editor_sequence=0;
        std::mutex weapon_mutex;
        std::deque<net::weapon::command> weapon_outgoing;
        std::deque<net::weapon::snapshot> weapon_updates;
        std::deque<net::weapon::shot> weapon_shots;
        std::uint32_t weapon_ack = 0;
        std::uint32_t weapon_seen_id=0,weapon_seen_sequence=0,weapon_seen_tick=0;
        std::thread worker;
        bool initialized = false;
    };
    state* get();
    std::shared_ptr<const ::scene::state> snapshot();
    std::optional<net::animation::resource> animation(std::uint32_t instance_id);
    bool send_editor_light(net::editor_light::resource command);
    bool send_weapon(net::weapon::command command);
    std::deque<net::weapon::snapshot> take_weapon_updates();
    std::deque<net::weapon::shot> take_weapon_shots();
    void check_error();
    void send_input(const ::player::input& input);
    std::optional<net::movement::snapshot> take_authority();
    std::optional<::player::parameters_t> movement_parameters();
    std::vector<net::movement::peer> remote_players();
    void init();
    void shutdown() noexcept;

using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    namespace {
        void run(const std::string& host, std::uint16_t port);
    }
    state* get() {
        static state data;
        return &data;
    }
    void reset_scene(state& data) {
        std::lock_guard lock(data.scene_mutex);
        data.scene.reset();
        data.pending_scene.clear();
        data.last_publish={};
        data.light_versions.fill(0);
        data.light_pending.fill(std::nullopt);
        {
            std::lock_guard peers_lock(data.peers_mutex);
            data.peers={};
            data.peers_received={};
        }
    }
    void reset_queues(state& data,bool reset_parameters) {
        {
            std::lock_guard lock(data.player_mutex);
            data.outgoing.clear();
            data.authority.reset();
            if(reset_parameters)data.server_parameters.reset();
        }
        {
            std::lock_guard lock(data.editor_mutex);
            data.editor_outgoing.clear();
            data.editor_sequence=0;
        }
        reset_scene(data);
    }
    std::shared_ptr<const scene::state> snapshot() {
        auto& data=*get();
        std::lock_guard lock(data.scene_mutex);
        std::size_t count=data.scene?data.scene->lights.size():0;
        for(const auto& batch:data.pending_scene) count+=batch.lights.size();
        bool pending_light=false;
        for(std::size_t i=0;i<data.light_pending.size()&&i<=count;++i)
            if(data.light_pending[i]) {pending_light=true;break;}
        if(data.pending_scene.empty()&&(!pending_light||!data.scene))
            return data.scene;
        const auto now=clock::now();
if(data.scene&&!data.scene_complete.load(std::memory_order_relaxed)&& now-data.last_publish<16ms) return data.scene;
        auto combined=data.scene
            ? std::make_shared<::scene::state>(*data.scene)
            : std::make_shared<::scene::state>();
        const auto append=[&](auto field) {
            auto& output=(*combined).*field;
            std::size_t total=output.size();
            for(const auto& patch:data.pending_scene)
                total+=(patch.*field).size();
            output.reserve(total);
            for(auto& patch:data.pending_scene) {
                auto& source=patch.*field;
output.insert(output.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
            }
        };
        append(&::scene::state::skins);
        append(&::scene::state::textures);
        append(&::scene::state::meshes);
        append(&::scene::state::instances);
        append(&::scene::state::collision_boxes);
        append(&::scene::state::lights);
        const auto light_value=[](const ::scene::point_light& l) {
return std::tuple{l.position.x,l.position.y,l.position.z, l.radius,l.color.x,l.color.y,l.color.z,l.strength,l.enabled};
        };
        bool changed=false;
        for(std::size_t i=0;i<data.light_pending.size();++i) {
            auto& pending=data.light_pending[i];
            if(!pending||i>combined->lights.size())continue;
            if(i==combined->lights.size()) {
                combined->lights.push_back(pending->light);
                changed=true;
            } else if(light_value(combined->lights[i])!=light_value(pending->light)) {
                combined->lights[i]=pending->light;
                changed=true;
            }
            pending.reset();
        }
        if(data.pending_scene.empty()&&!changed) return data.scene;
        data.scene=std::move(combined);
        data.pending_scene.clear();
        data.last_publish=now;
        return data.scene;
    }
    std::optional<net::animation::resource> animation(std::uint32_t instance_id) {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.animation_mutex);
        const auto it = data.animations.find(instance_id);
        if (it == data.animations.end()) return std::nullopt;
        return it->second;
    }
    bool send_editor_light(net::editor_light::resource command) {
if((command.type!=net::editor_light::kind::create && command.type!=net::editor_light::kind::edit && command.type!=net::editor_light::kind::remove) || !net::editor_light::valid(command.light) ||
           !get()->connected.load(std::memory_order_relaxed))
            return false;
        auto& data=*get();
        std::lock_guard<std::mutex> lock(data.editor_mutex);
        if(data.editor_outgoing.size()>=64)return false;
        command.sequence=++data.editor_sequence;
        if(!command.sequence)command.sequence=++data.editor_sequence;
command.index=command.type==net::editor_light::kind::create ? UINT32_MAX:command.index;
        data.editor_outgoing.push_back(command);
        return true;
    }
    bool send_weapon(net::weapon::command command) {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.weapon_mutex);
        if(data.weapon_outgoing.size()>=64)return false;
        data.weapon_outgoing.push_back(command);
        return true;
    }
    std::deque<net::weapon::snapshot> take_weapon_updates() {
        auto& data=*get();
        std::lock_guard lock(data.weapon_mutex);
        return std::exchange(data.weapon_updates,{});
    }
    std::deque<net::weapon::shot> take_weapon_shots() {
        auto& data=*get();
        std::lock_guard lock(data.weapon_mutex);
        return std::exchange(data.weapon_shots,{});
    }
    void check_error() {
        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(get()->error_mutex);
            error = get()->worker_error;
        }
        if (error) 
            std::rethrow_exception(error);
    }
    void send_input(const ::player::input& input) {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.player_mutex);
        if (data.outgoing.size() >= 128) 
            data.outgoing.pop_front();
        data.outgoing.push_back(input);
    }
    std::optional<net::movement::snapshot> take_authority() {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.player_mutex);
        return std::exchange(data.authority,std::nullopt);
    }
    std::optional<::player::parameters_t> movement_parameters() {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.player_mutex);
        return data.server_parameters;
    }
    std::vector<net::movement::peer> remote_players() {
        auto& data=*get();
        std::lock_guard lock(data.peers_mutex);
        if(clock::now()-data.peers_received>500ms)return {};
        return data.peers.players;
    }
    void init() {
        if (get()->initialized || get()->worker.joinable()) 
            throw std::runtime_error("clt::connection already initialized");
        reset_queues(*get(),true);
        {
            std::lock_guard<std::mutex> lock(get()->error_mutex);
            get()->worker_error = {};
        }
        get()->stop.store(false, std::memory_order_relaxed);
        get()->connected.store(false, std::memory_order_relaxed);
        get()->server_tick.store(0, std::memory_order_relaxed);
        get()->scene_complete.store(false, std::memory_order_relaxed);
        get()->worker = std::thread(run, get()->host, get()->port);
        get()->initialized = true;
    }
    void shutdown() noexcept {
        get()->stop.store(true, std::memory_order_relaxed);
        if (get()->worker.joinable()) 
            get()->worker.join();
        reset_queues(*get(),false);
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
    namespace {
        void run(const std::string& host, std::uint16_t port) {
            auto& data = *get();
            try {
                const auto server=net::endpoint::resolve(host,port);
data.socket=net::secure::create( net::secure::role::client, game::secure_config::load(net::secure::role::client,host,port),0, server.address.ss_family==AF_INET6?"::":"0.0.0.0");
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
                auto clear_scene = [&] {
                    {
                        std::lock_guard<std::mutex> lock(data.weapon_mutex);
                        data.weapon_outgoing.clear();
                        data.weapon_updates.clear();
                        data.weapon_shots.clear();
                        data.weapon_ack = 0;
                        data.weapon_seen_id=data.weapon_seen_sequence=data.weapon_seen_tick=0;
                    }
                    {
                        std::lock_guard<std::mutex> lock(data.editor_mutex);
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
                    reset_scene(data);
                    {
                        std::lock_guard<std::mutex> animation_lock(data.animation_mutex);
                        data.animations.clear();
                    }
                };
                auto send = [&](net::message::type type) {
                    const auto packet = net::message::encode({type, ++sequence, token, 0});
                    net::secure::send(data.socket, server, packet.data(), packet.size());
                    outstanding = sequence;
                };
                while (!data.stop.load(std::memory_order_relaxed)) {
                    const auto now = clock::now();
                    for (int budget = 0; budget < 512; ++budget) {
                        net::endpoint::resource peer;
                        const auto size = net::secure::receive(data.socket, peer, buffer.data(), buffer.size());
                        if (!size) 
                            break;
                        if(!net::endpoint::equal(peer,server))continue;
if(const auto update=net::editor_light::decode( buffer.data(),*size)) {
if(update->token==token && data.connected.load(std::memory_order_relaxed) && (update->type==net::editor_light::kind::state || update->type==net::editor_light::kind::ack)) {
                                if(update->type==net::editor_light::kind::ack) {
                                    std::lock_guard<std::mutex> lock(data.editor_mutex);
if(!data.editor_outgoing.empty() && data.editor_outgoing.front().sequence== update->sequence) {
                                        data.editor_outgoing.pop_front();
                                        std::clog<<"clt: editor light "<<update->index
                                                 <<" confirmed by server\n";
                                    }
                                }
                                {
                                    std::lock_guard<std::mutex> lock(data.scene_mutex);
                                    auto& revision=data.light_versions[update->index];
                                    if(net::message::newer(update->revision,revision)) {
                                        revision=update->revision;
                                        data.light_pending[update->index]=*update;
                                    }
                                }
                                last_received=now;
                            }
                            continue;
                        }
                        auto accept_weapon = [&](const net::weapon::snapshot& update) {
                            std::lock_guard<std::mutex> lock(data.weapon_mutex);
                            if(data.weapon_seen_id==update.weapon_id &&
                               (net::message::newer(data.weapon_seen_sequence,update.sequence) ||
                                (update.sequence==data.weapon_seen_sequence &&
                                 net::message::newer(data.weapon_seen_tick,update.tick))))
                                return false;
                            data.weapon_seen_id=update.weapon_id;
                            data.weapon_seen_sequence=update.sequence;
                            data.weapon_seen_tick=update.tick;
                            if (!net::message::newer(data.weapon_ack, update.sequence)) {
                                data.weapon_ack = update.sequence;
                            }
while (!data.weapon_outgoing.empty() && !net::message::newer( data.weapon_outgoing.front().sequence, data.weapon_ack )) 
                                data.weapon_outgoing.pop_front();
                            if (data.weapon_updates.size() >= 64) 
                                data.weapon_updates.pop_front();
                            data.weapon_updates.push_back(update);
                            last_received = now;
                            return true;
                        };
                        if (const auto shot = net::weapon::decode_shot(buffer.data(), *size)) {
if (shot->state.token == token && data.connected.load(std::memory_order_relaxed)) {
                                accept_weapon(shot->state);
                                std::lock_guard<std::mutex> lock(data.weapon_mutex);
                                if (data.weapon_shots.size() >= 64) 
                                    data.weapon_shots.pop_front();
                                data.weapon_shots.push_back(*shot);
                            }
                            continue;
                        }
                        if (const auto update = net::weapon::decode_snapshot(buffer.data(), *size)) {
if (update->token == token && data.connected.load(std::memory_order_relaxed)) 
                                accept_weapon(*update);
                            continue;
                        }
                        if (const auto state = net::animation::decode(buffer.data(), *size)) {
if (state->token == token && data.connected.load(std::memory_order_relaxed)) {
                                std::lock_guard<std::mutex> lock(data.animation_mutex);
                                const auto old = data.animations.find(state->instance_id);
if (old == data.animations.end() || net::message::newer(state->server_tick, old->second.server_tick))
                                    data.animations[state->instance_id] = *state;
                                last_received = now;
                            }
                            continue;
                        }
if (const auto settings = net::movement::decode_parameters( buffer.data(), *size)) {
if (settings->token == token && data.connected.load(std::memory_order_relaxed)) {
                                std::lock_guard<std::mutex> lock(data.player_mutex);
                                data.server_parameters = settings->values;
                                last_received = now;
                            }
                            continue;
                        }
                        if(const auto peers=net::movement::decode_peers(buffer.data(),*size)) {
                            if(peers->token==token && data.connected.load(std::memory_order_relaxed)) {
                                std::lock_guard lock(data.peers_mutex);
                                if(data.peers_received==clock::time_point{} ||
                                   net::message::newer(peers->tick,data.peers.tick)) {
                                    data.peers=*peers;
                                    data.peers_received=now;
                                }
                                last_received=now;
                            }
                            continue;
                        }
                        if (const auto update = net::movement::decode_snapshot(buffer.data(), *size)) {
if (update->token == token && data.connected.load(std::memory_order_relaxed)) {
                                last_received = now;
                                if (!net::message::newer(update->tick, last_player_tick)) 
                                    continue;
                                last_player_tick = update->tick;
while (!unacknowledged.empty() && !net::message::newer(unacknowledged.front().sequence, update->sequence)) 
                                    unacknowledged.pop_front();
                                std::lock_guard<std::mutex> lock(data.player_mutex);
                                data.authority = *update;
                            }
                            continue;
                        }
                        const auto message = net::message::decode(buffer.data(), *size);
if(transport.accept(peer,buffer.data(),*size,now)) 
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
transport.pump( [&](const net::endpoint::resource& peer,const void* bytes,std::size_t size) {
                            return net::secure::send(data.socket,peer,bytes,size);
                        },64*1024,now);
                    net::delivery completed;
                    while(transport.pop(completed)) {
                        net::scene_stream::batch_payload batch;
if(scene_ready || !net::scene_stream::decode_batch(completed.payload,batch) || batch.token!=token||batch.batch!=scene_batch)
                            continue;
                        if(stream_started==clock::time_point{})
                            stream_started=clock::now();
                        ::scene::state received;
                        if(!net::scene_stream::deserialize(batch.scene,received)) {
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
                        if(scene_ready && stream_started!=clock::time_point{}) {
const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>( clock::now()-stream_started).count();
                            std::cout<<"clt: scene transferred in "<<elapsed
                                     <<" ms ("<<transferred_bytes/1024<<" KiB)\n";
                        }
                    }
                    if (data.connected.load(std::memory_order_relaxed) && now - last_received > std::chrono::milliseconds(net::limits::connection_timeout_ms)) {
                        data.connected.store(false, std::memory_order_relaxed);
                        clear_scene();
                        net::secure::connect(data.socket,server);
                        const auto old_token = token;
                        do {
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
send(data.connected.load(std::memory_order_relaxed) ? net::message::type::ping : net::message::type::hello );
                        if (waiting) 
                            retry = std::min(retry * 2, std::chrono::milliseconds(net::limits::retry_max_ms));
                        waiting = true;
                        next_send = now + retry;
                    }
                    if (data.connected.load(std::memory_order_relaxed) && !scene_ready && now >= next_scene_send) {
                        const auto packet = net::message::encode({
net::message::type::scene_request, ++sequence, token, (scene_batch << 16)
                        });
net::secure::send( data.socket, server, packet.data(), packet.size() );
                        if(stream_started==clock::time_point{})
                            stream_started=now;
                        next_scene_send = now + std::chrono::milliseconds(net::limits::scene_request_ms);
                    }
                    if (data.connected.load(std::memory_order_relaxed) && now >= next_weapon_send) {
                        std::lock_guard<std::mutex> lock(data.weapon_mutex);
                        for (auto command : data.weapon_outgoing) {
                            command.token = token;
                            const auto packet = net::weapon::encode_command(command);
                            net::secure::send(data.socket, server, packet.data(), packet.size());
                        }
                        next_weapon_send = now + 30ms;
                    }
                    std::deque<::player::input> inputs;
                    {
                        std::lock_guard<std::mutex> lock(data.player_mutex);
                        for (int i = 0; i < 8 && !data.outgoing.empty(); ++i) {
                            inputs.push_back(data.outgoing.front());
                            data.outgoing.pop_front();
                        }
                    }
                    if (data.connected.load(std::memory_order_relaxed)) {
                        for (const auto& input : inputs) 
                            unacknowledged.push_back(input);
                        if (!unacknowledged.empty() && now >= next_move_send) {
                            auto transmit = [&](const ::player::input& input) {
                                const auto packet = net::movement::encode(token, input);
                                net::secure::send(data.socket, server, packet.data(), packet.size());
                            };
                            for (std::size_t i = 0; i < std::min<std::size_t>(8, unacknowledged.size()); ++i) 
                                transmit(unacknowledged[i]);
                            if (unacknowledged.size() > 8) 
                                transmit(unacknowledged.back());
                            next_move_send = now + std::chrono::milliseconds(net::limits::movement_ms);
                        }
                    } else {
                        unacknowledged.clear();
                    }
if(data.connected.load(std::memory_order_relaxed) && now>=next_editor_send) {
                        std::optional<net::editor_light::resource> pending;
                        {
                            std::lock_guard<std::mutex> lock(data.editor_mutex);
                            if(!data.editor_outgoing.empty())
                                pending=data.editor_outgoing.front();
                        }
                        if(pending) {
                            if(pending->sequence!=editor_retry_sequence) {
                                editor_retry_sequence=pending->sequence;
                                editor_retry_attempts=0;
                            }
                            if(editor_retry_attempts++<100u) {
                                pending->token=token;
                                const auto bytes=net::editor_light::encode(*pending);
net::secure::send(data.socket,server, bytes.data(),bytes.size());
                            } else {
                                std::lock_guard<std::mutex> lock(data.editor_mutex);
if(!data.editor_outgoing.empty() && data.editor_outgoing.front().sequence== pending->sequence)
                                    data.editor_outgoing.pop_front();
                                std::clog<<"clt: editor light request not acknowledged "
                                         <<"(check server editor permissions)\n";
                                editor_retry_sequence=0;
                                editor_retry_attempts=0;
                            }
                        } else {
                            editor_retry_sequence=0;
                            editor_retry_attempts=0;
                        }
                        next_editor_send=now+50ms;
                    }
                    auto wake = data.connected.load(std::memory_order_relaxed) && !scene_ready
                            ? std::min(next_send, next_scene_send)
                            : next_send;
                    if (!unacknowledged.empty()) 
                        wake = std::min(wake, next_move_send);
const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>( wake - clock::now() );
net::secure::wait_readable( data.socket, std::clamp( delay, 1ms, data.connected.load(std::memory_order_relaxed) ? 5ms : 50ms ) );
                }
                if (data.connected.load(std::memory_order_relaxed)) 
                    send(net::message::type::goodbye);
            } catch (...) {
                std::lock_guard<std::mutex> lock(data.error_mutex);
                data.worker_error = std::current_exception();
            }
            data.connected.store(false, std::memory_order_relaxed);
        }
    } // namespace

} // namespace clt::connection
