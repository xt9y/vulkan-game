#pragma once

#include "srv/game/options.hpp"

#include "srv/shared/net/message.hpp"
#include "srv/shared/net/socket.hpp"

#include "srv/shared/scene/scene.hpp"

#include "srv/world/world.hpp"

namespace srv::application
{
    struct state
    {
        game::options::state options;
        net::socket::resource socket;
        world::state terrain;
        ::scene::state scene;
        std::uint32_t tick = 0;
        bool initialized = false;
    };

    state* get();
    void init();
    int run();
    void shutdown() noexcept;
} // namespace srv::application
