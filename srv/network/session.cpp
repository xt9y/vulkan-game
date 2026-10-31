#include "session.hpp"

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
    }

    void expire(std::chrono::steady_clock::time_point now)
    {
        for (auto& client : get()->clients)
        {
            if (client.active && now - client.last_seen > std::chrono::seconds(5))
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
