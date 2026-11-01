#pragma once

#include <vector>
#include <memory>
#include <cstddef>

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
        std::shared_ptr<scene::state> importing;
        std::size_t next_texture = 0;
        player::collision_world colliders;
        spatial::index collision_index;
        std::vector<std::uint8_t> scene_bytes;
        std::uint32_t scene_hash = 0;
        struct batch {
            std::vector<std::uint8_t> bytes;
            std::uint32_t hash = 0;
            bool final = false;
        };
        std::vector<batch> scene_batches;
        std::uint32_t tick = 0;
        bool initialized = false;
    };

    state* get();
    void init();
    // Publish append-only mesh/instance batches; called on the server thread.
    // Background jobs return a completion that invokes append_scene().
    void begin_stream();
    void append_scene(::scene::state patch, bool final = false);
    int run();
    void shutdown() noexcept;
} // namespace srv::application
