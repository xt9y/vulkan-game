#include "session.hpp"
#include "srv/shared/net/limits.hpp"

namespace srv::session
{
    state* get()
    {
        static state data;
        return &data;
    }

    void init()
    {
        *get() = {};
        get()->clients.resize(net::limits::max_clients);
    }

    void expire(std::chrono::steady_clock::time_point now)
    {
        for (auto& client : get()->clients)
        {
            if (client.active && now - client.last_seen > std::chrono::seconds(net::limits::timeout_seconds))
            {
                client.active = false;
            }
        }
    }

    void shutdown()
    {
        *get() = {};
    }
} // namespace srv::session
