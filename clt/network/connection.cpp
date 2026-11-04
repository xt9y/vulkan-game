#include "connection.hpp"
#include "srv/shared/net/limits.hpp"
#include <iostream>

#include "srv/shared/net/endpoint.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/movement.hpp"
#include "srv/shared/net/socket.hpp"

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
        if (data.pending_scene.empty()) return data.scene;

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
        for (const auto& patch : data.pending_scene)
        {
            skins += patch.skins.size();
            textures += patch.textures.size();
            meshes += patch.meshes.size();
            instances += patch.instances.size();
        }
        combined->skins.reserve(skins);
        combined->textures.reserve(textures);
        combined->meshes.reserve(meshes);
        combined->instances.reserve(instances);
        if (data.scene)
        {
            combined->skins = data.scene->skins;
            combined->textures = data.scene->textures;
            combined->meshes = data.scene->meshes;
            combined->instances = data.scene->instances;
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
        }
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

    std::vector<net::blood::event> take_blood_events()
    {
        auto& data = *get();
        std::lock_guard<std::mutex> lock(data.blood_mutex);
        std::vector<net::blood::event> result(
            data.blood_events.begin(), data.blood_events.end());
        data.blood_events.clear();
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
            std::lock_guard<std::mutex> lock(get()->blood_mutex);
            get()->blood_events.clear();
            get()->blood_history.clear();
            get()->blood_seen.clear();
        }
        {
            std::lock_guard<std::mutex> lock(get()->scene_mutex);
            get()->scene.reset();
            get()->pending_scene.clear();
            get()->last_publish = {};
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
            std::lock_guard<std::mutex> lock(get()->blood_mutex);
            get()->blood_events.clear();
            get()->blood_history.clear();
            get()->blood_seen.clear();
        }
        {
            std::lock_guard<std::mutex> lock(get()->scene_mutex);
            get()->scene.reset();
            get()->pending_scene.clear();
            get()->last_publish = {};
        }

        net::socket::destroy(get()->socket);
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
                data.socket = net::socket::create();
                const auto server = net::endpoint::resolve(host, port);
                std::uint32_t token = std::random_device{}() | 1u;

                std::uint32_t sequence = 0;
                std::uint32_t outstanding = 0;
                std::uint32_t last_accepted = 0;
                bool waiting = false;
                
                std::uint32_t last_player_tick = 0;
                std::deque<::player::input> unacknowledged;

                std::vector<std::uint8_t> scene_bytes;
                std::vector<std::uint8_t> scene_received;
                std::uint32_t scene_fragment = 0;
                std::uint32_t reported_percent = 0;
                std::uint32_t scene_batch = 0;
                std::uint32_t scene_hash = 0;
                std::uint32_t requested_until = 0;
                bool scene_last = false;
                clock::time_point stream_started{};
                std::size_t transferred_bytes = 0;
                bool scene_ready = false;

                auto next_send = clock::now();
                auto next_scene_send = next_send;
                auto next_move_send = next_send;
                auto next_weapon_send = next_send;
                auto last_received = next_send;
                auto retry = std::chrono::milliseconds(net::limits::retry_ms);
                std::array<std::uint8_t, net::scene_stream::packet_size + 1> buffer{};

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
                    scene_bytes.clear();
                    scene_received.clear();
                    stream_started = {};
                    transferred_bytes = 0;
                    {
                        std::lock_guard<std::mutex> lock(data.player_mutex);
                        data.server_parameters.reset();
                    }
                    scene_fragment = 0;
                    reported_percent = 0;
                    scene_batch = 0;
                    scene_hash = 0;
                    requested_until = 0;
                    scene_last = false;
                    scene_ready = false;
                    data.scene_complete.store(false, std::memory_order_relaxed);

                    std::lock_guard<std::mutex> lock(data.scene_mutex);
                    data.scene.reset();
                    data.pending_scene.clear();
                    data.last_publish = {};
                    {
                        std::lock_guard<std::mutex> blood_lock(data.blood_mutex);
                        data.blood_events.clear();
                        data.blood_history.clear();
                        data.blood_seen.clear();
                    }
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
                    net::socket::send(data.socket, server, packet.data(), packet.size());
                    outstanding = sequence;
                };

                while (!data.stop.load(std::memory_order_relaxed))
                {
                    const auto now = clock::now();

                    for (int budget = 0; budget < 512; ++budget)
                    {
                        net::endpoint::resource peer;
                        const auto size = net::socket::receive(data.socket, peer, buffer.data(), buffer.size());

                        if (!size)
                        {
                            break;
                        }

                        if (const auto effect = net::blood::decode(buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) &&
                                effect->token == token &&
                                data.connected.load(std::memory_order_relaxed))
                            {
                                std::lock_guard<std::mutex> lock(data.blood_mutex);
                                if (data.blood_seen.insert(effect->id).second)
                                {
                                    if (data.blood_history.size() >= 512)
                                    {
                                        data.blood_seen.erase(data.blood_history.front());
                                        data.blood_history.pop_front();
                                    }
                                    data.blood_history.push_back(effect->id);
                                    if (data.blood_events.size() >= 256)
                                        data.blood_events.pop_front();
                                    data.blood_events.push_back(*effect);
                                }
                                last_received = now;
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

                        net::scene_stream::fragment fragment;

                        if (net::endpoint::equal(peer, server) &&
                            net::scene_stream::decode_fragment(buffer.data(), *size, fragment))
                        {
                            if (!data.connected.load(std::memory_order_relaxed) || scene_ready ||
                                fragment.token != token || (fragment.index >> 16) != scene_batch)
                            {
                                continue;
                            }

                            last_received = now;
                            const auto index = fragment.index & 0xffffu;

                            if (scene_bytes.empty())
                            {
                                if (stream_started == clock::time_point{})
                                    stream_started = clock::now();
                                scene_bytes.resize(fragment.total);
                                scene_received.assign(
                                    (fragment.total + net::scene_stream::fragment_size - 1) /
                                    net::scene_stream::fragment_size, 0
                                );
                                scene_hash = fragment.checksum;
                                reported_percent = 0;
                                scene_last = fragment.final;
                            }
                            else if (fragment.total != scene_bytes.size() ||
                                     fragment.checksum != scene_hash ||
                                     fragment.final != scene_last)
                            {
                                clear_scene();
                                next_scene_send = now;
                                continue;
                            }

                            if (index >= scene_received.size())
                            {
                                continue;
                            }

                            if (!scene_received[index])
                            {
                                std::memcpy(
                                    scene_bytes.data() +
                                        std::size_t(index) * net::scene_stream::fragment_size,
                                    fragment.data, fragment.size
                                );
                                scene_received[index] = 1;
                                transferred_bytes += fragment.size;

                                while (scene_fragment < scene_received.size() &&
                                       scene_received[scene_fragment])
                                {
                                    ++scene_fragment;
                                }

                                const auto progress = std::uint32_t(
                                    std::uint64_t(scene_fragment) * 100 /
                                    std::max<std::size_t>(1, scene_received.size()));
                                if (progress >= reported_percent + 10)
                                {
                                    reported_percent = (progress / 10) * 10;
                                    std::cout << "clt: scene batch " << scene_batch
                                              << " " << reported_percent << "%\n";
                                }
                                // A complete burst unlocks the next window immediately.
                                if (scene_fragment >= requested_until)
                                {
                                    next_scene_send = now;
                                }
                            }

                            if (scene_fragment == scene_received.size())
                            {
                                scene::state received;

                                if (net::scene_stream::checksum(scene_bytes) == scene_hash &&
                                    net::scene_stream::deserialize(scene_bytes, received))
                                {
                                    {
                                        std::lock_guard<std::mutex> lock(data.scene_mutex);
                                        std::cout << "clt: received scene batch " << scene_batch
                                                   << " (" << received.meshes.size() << " meshes, "
                                                   << received.instances.size() << " instances, "
                                                   << received.textures.size() << " textures, "
                                                   << received.skins.size() << " skins)\n";
                                        data.pending_scene.push_back(std::move(received));
                                    }

                                    scene_ready = scene_last;
                                    if (scene_ready && stream_started != clock::time_point{})
                                    {
                                        const auto elapsed =
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                clock::now() - stream_started).count();
                                        std::cout << "clt: scene transferred in " << elapsed
                                                  << " ms (" << transferred_bytes / 1024
                                                  << " KiB)\n";
                                    }
                                    data.scene_complete.store(scene_ready, std::memory_order_relaxed);
                                    ++scene_batch;
                                    scene_fragment = 0;
                                    reported_percent = 0;
                                    requested_until = 0;
                                    scene_hash = 0;
                                    scene_last = false;
                                    scene_received.clear();
                                    scene_bytes.clear();
                                    next_scene_send = now;

                                }
                                else
                                {
                                    clear_scene();
                                    next_scene_send = now;
                                }
                            }

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

                    if (data.connected.load(std::memory_order_relaxed) && now - last_received > std::chrono::milliseconds(net::limits::connection_timeout_ms))
                    {
                        data.connected.store(false, std::memory_order_relaxed);
                        clear_scene();

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
                            (scene_batch << 16) | scene_fragment
                        });

                        net::socket::send(
                            data.socket,
                            server,
                            packet.data(),
                            packet.size()
                        );

                        requested_until = scene_fragment + net::limits::fragments_per_tick;
                        next_scene_send = now + std::chrono::milliseconds(net::limits::scene_request_ms);
                    }

                    if (data.connected.load(std::memory_order_relaxed) && now >= next_weapon_send)
                    {
                        std::lock_guard<std::mutex> lock(data.weapon_mutex);
                        for (auto command : data.weapon_outgoing)
                        {
                            command.token = token;
                            const auto packet = net::weapon::encode_command(command);
                            net::socket::send(data.socket, server, packet.data(), packet.size());
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
                                net::socket::send(data.socket, server, packet.data(), packet.size());
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

                    net::socket::wait_readable(
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
