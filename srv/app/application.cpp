#include "application.hpp"

#include "srv/game/application.hpp"
#include "srv/network/session.hpp"

#include "srv/shared/net/endpoint.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/scene_stream.hpp"
#include "srv/shared/net/movement.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/player/movement.hpp"
#include "srv/world/worker.hpp"

#include <algorithm>

#include "srv/scene/test.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>

namespace srv::application
{
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    namespace
    {
        void receive_messages();
        void send_scene_fragments();
        void handle(const net::endpoint::resource& peer, const net::message::resource& message);
        void reply(
            const net::endpoint::resource& peer,
            net::message::type type,
            const net::message::resource& request
        );
        void tick_players();
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
            throw std::runtime_error("srv::application already initialized");
        }

        get()->options = *game::options::get();
        get()->socket = net::socket::create(get()->options.port, get()->options.bind_host);
        session::init();
        scene::build_test(get()->scene);
        get()->colliders = player::build_colliders(get()->scene);
        get()->collision_index = player::build_index(get()->colliders);
        srv::generation::init();
        get()->scene_bytes = net::scene_stream::serialize(get()->scene);
        get()->scene_hash = net::scene_stream::checksum(get()->scene_bytes);
        get()->scene_batches = {{get()->scene_bytes, get()->scene_hash, true}};
        world::init(get()->terrain, 123456789ULL);
        get()->tick = 0;
        get()->initialized = true;
    }

    void begin_stream()
    {
        auto& data = *get();
        data.scene_bytes = net::scene_stream::serialize(data.scene);
        data.scene_hash = net::scene_stream::checksum(data.scene_bytes);
        data.scene_batches = {{data.scene_bytes, data.scene_hash, false}};
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
        auto hash = net::scene_stream::checksum(bytes);

        for (auto& mesh : patch.meshes)
        {
            data.scene.meshes.push_back(std::move(mesh));
        }

        // Only the new geometry needs collision bounds and a new BVH root.
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
        data.colliders.insert(
            data.colliders.end(), added_colliders.begin(), added_colliders.end()
        );
        data.collision_index.append(boxes);
        data.scene_batches.push_back({std::move(bytes), hash, final});
    }

    int run()
    {
        if (!get()->initialized)
        {
            throw std::runtime_error("srv::application is not initialized");
        }

        std::cout << "Server listening on " << get()->options.bind_host << ':'
                  << get()->options.port << " (UDP, 60 Hz)\n"
                  << std::flush;
        const auto started = clock::now();
        auto next = started;
        constexpr auto interval = std::chrono::nanoseconds(1'000'000'000 / 60);

        // auto ground = srv::world::height(get()->terrain, 10, 20);
        // bool blocked = srv::world::solid(get()->terrain, 10.5, 12.0, 20.5);
        // srv::world::aabb player{
        //     10.2, 12.0, 20.2,
        //     10.8, 13.8, 20.8
        // };
        // bool collision = srv::world::collides(get()->terrain, player);
        // srv::world::set_height(get()->terrain, 10, 20, 25);
        // std::int32_t heights[16 * 16];
        // srv::world::sample(get()->terrain, 0, 0, 16, 16, heights);
        //
        // std::cout << "Ground: " << ground
        //       << ", Blocked: " << blocked
        //       << ", Collision: " << collision << '\n';

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
            send_scene_fragments();

            // Streaming generation may be expensive; commit only when there is
            // room before the next tick, without blocking player simulation.
            if (clock::now() + 2ms < next + interval)
            {
                generation::commit();
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
        net::socket::destroy(get()->socket);
        session::shutdown();
        generation::shutdown();
        get()->scene_bytes.clear();
        get()->scene_batches.clear();
        get()->colliders.clear();
        get()->scene_hash = 0;
        get()->scene = {};
        get()->terrain = {};
        get()->tick = 0;
        get()->initialized = false;
    }

    namespace
    {
        void receive_messages()
        {
            // One extra byte ensures oversized datagrams can never look like valid packets after
            // truncation.
            std::array<std::uint8_t, 25> buffer{};

            const auto deadline = clock::now() + 2ms;
            for (int budget = 0; budget < 96 && clock::now() < deadline; ++budget)
            {
                net::endpoint::resource peer;
                const auto size = net::socket::receive(get()->socket, peer, buffer.data(), buffer.size());

                if (!size)
                {
                    break;
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
                    *client = session::resource{};
                    client->endpoint = peer;
                    client->token = message.token;
                    client->active = true;
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
                // One pending request per session; never spend physics time
                // transmitting bulk scene packets from the receive loop.
                existing->scene_request = message.value;
                existing->scene_pending = true;
            }
        }

        void send_scene_fragments()
        {
            constexpr int max_fragments = 32;
            const auto deadline = clock::now() + 2ms;
            int remaining = max_fragments;

            for (auto& client : session::get()->clients)
            {
                if (!client.active || !client.scene_pending || !remaining ||
                    clock::now() >= deadline)
                {
                    continue;
                }
                client.scene_pending = false;

                const auto batch_id = client.scene_request >> 16;
                const auto fragment_id = client.scene_request & 0xffffu;
                if (batch_id >= get()->scene_batches.size())
                {
                    continue;
                }

                const auto& batch = get()->scene_batches[batch_id];
                const auto count = (batch.bytes.size() +
                    net::scene_stream::fragment_size - 1) /
                    net::scene_stream::fragment_size;

                for (std::size_t i = fragment_id;
                     i < std::min<std::size_t>(count, fragment_id + 32) &&
                     remaining && clock::now() < deadline; ++i)
                {
                    net::scene_stream::packet packet;
                    const auto size = net::scene_stream::encode_fragment(
                        batch.bytes, client.token, static_cast<std::uint32_t>(i),
                        batch.hash, packet, batch.final
                    );
                    if (!size)
                    {
                        break;
                    }

                    const auto index = (batch_id << 16) |
                        static_cast<std::uint32_t>(i);
                    packet[16] = std::uint8_t(index >> 24);
                    packet[17] = std::uint8_t(index >> 16);
                    packet[18] = std::uint8_t(index >> 8);
                    packet[19] = std::uint8_t(index);

                    if (!net::socket::send(
                        get()->socket, client.endpoint, packet.data(), size))
                    {
                        break; // The client retries from its missing index.
                    }
                    --remaining;
                }
            }
        }

        void reply(const net::endpoint::resource& peer, net::message::type type, const net::message::resource& request)
        {
            const auto data =
                net::message::encode({type, request.sequence, request.token, get()->tick});
            net::socket::send(get()->socket, peer, data.data(), data.size());
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
                for (int budget = 0; budget < 4 && !client.inputs.empty(); ++budget)
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

                net::socket::send(get()->socket, client.endpoint, packet.data(), packet.size());
            }
        }
    } // namespace
} // namespace srv::application
