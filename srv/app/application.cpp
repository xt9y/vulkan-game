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
        srv::generation::init();
        get()->scene_bytes = net::scene_stream::serialize(get()->scene);
        get()->scene_hash = net::scene_stream::checksum(get()->scene_bytes);
        world::init(get()->terrain, 123456789ULL);
        get()->tick = 0;
        get()->initialized = true;
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

            receive_messages();
            tick_players();
            generation::commit();
            next += interval;

            if (next < clock::now())
            {
                next = clock::now(); // Skip missed ticks rather than accumulate catch-up work.
            }

            std::this_thread::sleep_until(next);
        }

        return 0;
    }

    void shutdown() noexcept
    {
        net::socket::destroy(get()->socket);
        session::shutdown();
        generation::shutdown();
        get()->scene_bytes.clear();
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

            for (int budget = 0; budget < 256; ++budget)
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

                        if (duplicate == client.inputs.end() && client.inputs.size() < 128)
                        {
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
                    *client = session::resource{peer, message.token, 0, false, true, clock::now()};
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

                net::scene_stream::packet packet;

                const auto size = net::scene_stream::encode_fragment(get()->scene_bytes, message.token, message.value, get()->scene_hash, packet);
                
                if (size)
                {
                    net::socket::send(get()->socket, peer, packet.data(), size);
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
                    player::step(client.avatar, input, get()->colliders);
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
