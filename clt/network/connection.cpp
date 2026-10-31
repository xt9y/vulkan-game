#include "connection.hpp"

#include "srv/shared/net/endpoint.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/movement.hpp"
#include "srv/shared/net/socket.hpp"

#include "srv/shared/net/scene_stream.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <algorithm>
#include <chrono>
#include <iostream>
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
        std::lock_guard<std::mutex> lock(get()->scene_mutex);
        return get()->scene;
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
        }

        {
            std::lock_guard<std::mutex> lock(get()->scene_mutex);
            get()->scene.reset();
        }

        get()->stop.store(false, std::memory_order_relaxed);
        get()->connected.store(false, std::memory_order_relaxed);
        get()->server_tick.store(0, std::memory_order_relaxed);
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
            std::lock_guard<std::mutex> lock(get()->scene_mutex);
            get()->scene.reset();
        }

        net::socket::destroy(get()->socket);
        get()->connected.store(false, std::memory_order_relaxed);
        get()->server_tick.store(0, std::memory_order_relaxed);
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
                const std::uint32_t token = std::random_device{}() | 1u;

                std::uint32_t sequence = 0;
                std::uint32_t outstanding = 0;
                std::uint32_t last_accepted = 0;
                bool waiting = false;
                
                std::uint32_t last_player_tick = 0;
                std::deque<::player::input> recent_inputs;

                std::vector<std::uint8_t> scene_bytes;
                std::uint32_t scene_fragment = 0;
                std::uint32_t scene_hash = 0;
                bool scene_ready = false;

                auto next_send = clock::now();
                auto next_scene_send = next_send;
                auto last_received = next_send;
                auto retry = 250ms;
                std::array<std::uint8_t, net::scene_stream::packet_size + 1> buffer{};
                
                auto clear_scene = [&]
                {
                    scene_bytes.clear();
                    scene_fragment = 0;
                    scene_hash = 0;
                    scene_ready = false;

                    std::lock_guard<std::mutex> lock(data.scene_mutex);
                    data.scene.reset();
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

                    for (int budget = 0; budget < 64; ++budget)
                    {
                        net::endpoint::resource peer;
                        const auto size = net::socket::receive(data.socket, peer, buffer.data(), buffer.size());

                        if (!size)
                        {
                            break;
                        }
                        
                        if (const auto update = net::movement::decode_snapshot(buffer.data(), *size))
                        {
                            if (net::endpoint::equal(peer, server) && update->token == token &&
                                data.connected.load(std::memory_order_relaxed) &&
                                net::message::newer(update->tick, last_player_tick))
                            {
                                last_player_tick = update->tick;
                                std::lock_guard<std::mutex> lock(data.player_mutex);
                                data.authority = *update;
                            }

                            continue;
                        }

                        const auto message = net::message::decode(buffer.data(), *size);

                        net::scene_stream::fragment fragment;

                        if (net::endpoint::equal(peer, server) &&
                            net::scene_stream::decode_fragment(buffer.data(),*size, fragment))
                        {
                            if (!data.connected.load(std::memory_order_relaxed) || scene_ready ||
                                fragment.token != token ||
                                fragment.index != scene_fragment)
                            {
                                continue;
                            }

                            if (scene_fragment == 0)
                            {
                                scene_bytes.resize(fragment.total);
                                scene_hash = fragment.checksum;
                            }
                            else if (fragment.total != scene_bytes.size() || fragment.checksum != scene_hash)
                            {
                                clear_scene();
                                next_scene_send = now;
                                continue;
                            }

                            std::memcpy(
                                scene_bytes.data() + std::size_t(scene_fragment) * net::scene_stream::fragment_size,
                                fragment.data,
                                fragment.size
                            );

                            ++scene_fragment;
                            next_scene_send = now;

                            if (std::size_t(scene_fragment) * net::scene_stream::fragment_size >= scene_bytes.size())
                            {
                                scene::state received;

                                if (net::scene_stream::checksum(scene_bytes) == scene_hash &&
                                    net::scene_stream::deserialize(scene_bytes, received))
                                {
                                    auto completed = std::make_shared<::scene::state>(std::move(received));

                                    std::cout << "Scene received: "
                                              << completed->meshes.size() << " meshes, "
                                              << completed->instances.size() << " instances\n";

                                    {
                                        std::lock_guard<std::mutex> lock(data.scene_mutex);
                                        data.scene = std::move(completed);
                                    }

                                    scene_ready = true;
                                    scene_bytes.clear();
                                }
                                else
                                {
                                    clear_scene();
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
                        next_send = now + 50ms;
                        waiting = false;
                        retry = 250ms;
                    }

                    if (data.connected.load(std::memory_order_relaxed) && now - last_received > 3s)
                    {
                        data.connected.store(false, std::memory_order_relaxed);
                        clear_scene();

                        last_player_tick = 0;
                        recent_inputs.clear();
                        {
                            std::lock_guard<std::mutex> lock(data.player_mutex);
                            data.outgoing.clear();
                            data.authority.reset();
                        }        

                        waiting = false;
                        next_send = now;
                        retry = 250ms;
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
                            retry = std::min(retry * 2, 1000ms);
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
                            scene_fragment
                        });

                        net::socket::send(
                            data.socket,
                            server,
                            packet.data(),
                            packet.size()
                        );

                        next_scene_send = now + 100ms;
                    }

                    const auto wake = data.connected.load(std::memory_order_relaxed) && !scene_ready
                            ? std::min(next_send, next_scene_send)
                            : next_send;

                    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                        wake - clock::now()
                    );

                    net::socket::wait_readable(
                        data.socket,
                        std::clamp(delay, 1ms, 50ms)
                    );
                }

                if (data.connected.load(std::memory_order_relaxed))
                {
                    send(net::message::type::goodbye);
                }
            }
            catch (const std::exception& error)
            {
                std::cerr << "Server connection: " << error.what() << '\n';
            }

            data.connected.store(false, std::memory_order_relaxed);
        }
    } // namespace
} // namespace clt::connection
