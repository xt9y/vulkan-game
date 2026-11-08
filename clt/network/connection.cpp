#include "connection.hpp"
#include "srv/shared/net/limits.hpp"
#include <iostream>

#include <network/endpoint.hpp>
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/movement.hpp"
#include <network/secure.hpp>
#include "srv/shared/net/secure_config.hpp"
#include <network/session.hpp>

#include "srv/shared/net/scene_stream.hpp"

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
