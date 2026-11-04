#pragma once

#include "srv/game/options.hpp"
#include "srv/shared/net/animation.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/net/weapon.hpp"
#include "srv/shared/player/movement.hpp"
#include "srv/shared/scene/scene.hpp"
#include "srv/shared/weapon/weapon.hpp"
#include "srv/shared/weapon/hitbox.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>

namespace srv::application
{
    struct model_request
    {
        std::filesystem::path path;
        ::scene::transform pose;
        std::uint32_t animation;
        bool flesh = false;
    };

    struct weapon_command_context
    {
        const net::weapon::command& command;
        const ::weapon::definition& definition;
        ::weapon::state& state;
        const ::player::state& player;
        double now;
        const ::weapon::world& world;
        const std::unordered_set<std::uint32_t>& flesh_instances;
        const std::vector<::weapon::hitbox::capsule>& flesh_capsules;
    };

    using weapon_command_handler = std::function<::weapon::hit(weapon_command_context&)>;

    struct state
    {
        game::options::state options;
        net::socket::resource socket;
        scene::state scene;
        std::vector<model_request> models;
        bool scene_configured = false;
        std::shared_ptr<scene::state> importing;
        std::size_t next_texture = 0;
        std::size_t next_skin = 0;
        std::size_t next_mesh = 0;
        std::size_t next_instance = 0;
        std::size_t animation_cursor = 0;
        struct pending_event { net::animation::attachment_event event; std::uint32_t repeats = 3; };
        std::vector<pending_event> attachment_events;
        std::uint32_t next_event = 0;
        player::collision_world colliders;
        ::weapon::world weapon_world;
        std::unordered_set<std::uint32_t> flesh_instances;
        std::vector<::weapon::hitbox::capsule> flesh_capsules;
        std::uint32_t next_blood_event = 0;
        ::weapon::registry weapons;
        weapon_command_handler on_weapon_command;
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
    // The game defines both its initial scene and model imports before init().
    void set_scene(::scene::state scene);
    void load_model(std::filesystem::path path, ::scene::transform pose,
                    std::uint32_t animation, bool flesh = false);
    // Register game rules and receive authenticated, owned, equipped weapon commands.
    void add_weapon(::weapon::definition definition);
    void on_weapon_command(weapon_command_handler handler);
    void init();
    // Publish append-only mesh/instance batches; called on the server thread.
    // Background jobs return a completion that invokes append_scene().
    void begin_stream();
    void append_scene(::scene::state patch, bool final = false);
    // Call on the authoritative server tick (not from an import worker).
    bool play_animation(std::uint32_t instance_id, std::uint32_t clip,
                        std::uint32_t overlay = UINT32_MAX, float blend = 0.0f,
                        std::uint32_t mask_root = UINT32_MAX);
    // Send a named game effect at a skeleton socket; replicated to clients.
    bool emit_attachment_event(std::uint32_t instance_id, std::uint32_t joint,
                               std::uint32_t kind);
    int run();
    void shutdown() noexcept;
} // namespace srv::application
