#include "connection.hpp"

#include "srv/shared/net/message.hpp"
#include "srv/shared/net/socket.hpp"

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

    void init()
    {
        if (get()->initialized || get()->worker.joinable())
        {
            throw std::runtime_error("clt::connection already initialized");
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

                auto next_send = clock::now();
                auto last_received = next_send;
                auto retry = 250ms;
                std::array<std::uint8_t, net::message::packet_size + 1> buffer{};

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
                        const auto size =
                            net::socket::receive(data.socket, peer, buffer.data(), buffer.size());

                        if (!size)
                        {
                            break;
                        }

                        const auto message = net::message::decode(buffer.data(), *size);

                        if (!message || !net::endpoint::equal(peer, server) ||
                            message->token != token ||
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
                        waiting = false;
                        next_send = now;
                        retry = 250ms;
                    }

                    if (now >= next_send)
                    {
                        send(
                            data.connected.load(std::memory_order_relaxed)
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

                    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                        next_send - clock::now()
                    );
                    net::socket::wait_readable(data.socket, std::clamp(delay, 1ms, 50ms));
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
