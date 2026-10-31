#pragma once

#include <vector>

#include "srv/game/options.hpp"

#include "srv/shared/net/message.hpp"
#include "srv/shared/net/socket.hpp"

#include "srv/shared/scene/scene.hpp"
#include "srv/shared/player/movement.hpp"

#include "srv/world/world.hpp"

namespace srv::application
{
    struct state
    {
        game::options::state options;
        net::socket::resource socket;
        world::state terrain;
        scene::state scene;
        player::collision_world colliders;
        spatial::index collision_index;
        std::vector<std::uint8_t> scene_bytes;
        std::uint32_t scene_hash = 0;
        std::uint32_t tick = 0;
        bool initialized = false;
    };

    state* get();
    void init();
    int run();
    void shutdown() noexcept;
} // namespace srv::application
