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
        std::size_t meshes = data.scene ? data.scene->meshes.size() : 0;
        std::size_t instances = data.scene ? data.scene->instances.size() : 0;
        for (const auto& patch : data.pending_scene)
        {
            meshes += patch.meshes.size();
            instances += patch.instances.size();
        }
        combined->meshes.reserve(meshes);
        combined->instances.reserve(instances);
        if (data.scene)
        {
            combined->meshes = data.scene->meshes;
            combined->instances = data.scene->instances;
        }
        for (auto& patch : data.pending_scene)
        {
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
                std::uint32_t scene_batch = 0;
                std::uint32_t scene_hash = 0;
                std::uint32_t requested_until = 0;
                bool scene_last = false;
                bool scene_ready = false;

                auto next_send = clock::now();
                auto next_scene_send = next_send;
                auto next_move_send = next_send;
                auto last_received = next_send;
                auto retry = 250ms;
                std::array<std::uint8_t, net::scene_stream::packet_size + 1> buffer{};
                
                auto clear_scene = [&]
                {
                    scene_bytes.clear();
                    scene_received.clear();
                    scene_fragment = 0;
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
                                scene_bytes.resize(fragment.total);
                                scene_received.assign(
                                    (fragment.total + net::scene_stream::fragment_size - 1) /
                                    net::scene_stream::fragment_size, 0
                                );
                                scene_hash = fragment.checksum;
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

                                while (scene_fragment < scene_received.size() &&
                                       scene_received[scene_fragment])
                                {
                                    ++scene_fragment;
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
                                        data.pending_scene.push_back(std::move(received));
                                    }

                                    scene_ready = scene_last;
                                    data.scene_complete.store(scene_ready, std::memory_order_relaxed);
                                    ++scene_batch;
                                    scene_fragment = 0;
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
                        next_send = now + 50ms;
                        waiting = false;
                        retry = 250ms;
                    }

                    if (data.connected.load(std::memory_order_relaxed) && now - last_received > 3s)
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
                            (scene_batch << 16) | scene_fragment
                        });

                        net::socket::send(
                            data.socket,
                            server,
                            packet.data(),
                            packet.size()
                        );

                        requested_until = scene_fragment + 32;
                        next_scene_send = now + 40ms;
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

                            next_move_send = now + 16ms;
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
