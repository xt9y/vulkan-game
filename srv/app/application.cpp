#include "application.hpp"

#include "srv/game/application.hpp"
#include "srv/network/session.hpp"

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
        get()->tick = 0;
        get()->initialized = false;
    }

    namespace
    {
        void receive_messages()
        {
            // One extra byte ensures oversized datagrams can never look like valid packets after
            // truncation.
            std::array<std::uint8_t, net::message::packet_size + 1> buffer{};

            for (int budget = 0; budget < 256; ++budget)
            {
                net::endpoint::resource peer;
                const auto size =
                    net::socket::receive(get()->socket, peer, buffer.data(), buffer.size());

                if (!size)
                {
                    break;
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
        }

        void reply(
            const net::endpoint::resource& peer,
            net::message::type type,
            const net::message::resource& request
        )
        {
            const auto data =
                net::message::encode({type, request.sequence, request.token, get()->tick});
            net::socket::send(get()->socket, peer, data.data(), data.size());
        }
    } // namespace
} // namespace srv::application
