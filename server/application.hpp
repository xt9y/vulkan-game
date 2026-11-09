#pragma once
#include "scene.hpp"
#include "weapon.hpp"
#include "player.hpp"
#include <network/secure.hpp>
#include <network/session.hpp>
#include <spatial/index.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace srv::application {
struct model_request {
        std::filesystem::path path;
        ::scene::transform pose;
        std::uint32_t animation;
    };
    struct state {
        net::secure::resource socket;
        std::unique_ptr<net::session> scene_transport;
        scene::state scene;
        std::vector<std::uint32_t> editor_light_versions;
        std::uint32_t editor_light_revision=1;
        std::vector<model_request> models;
        bool scene_configured = false;
        std::shared_ptr<scene::state> importing;
        enum import_slot { texture_slot, skin_slot, mesh_slot, instance_slot };
        std::array<std::size_t,4> import_cursor{};
        std::size_t animation_cursor = 0;
        player::collision_world colliders;
        ::weapon::world weapon_world;
        ::weapon::definition weapon{};
        spatial::index collision_index;
        struct batch {
            std::vector<std::uint8_t> bytes;
            bool final=false;
        };
        std::vector<batch> scene_batches;
        std::uint32_t tick = 0;
        bool initialized = false;
    };
    state* get();
    void set_scene(::scene::state scene);
void load_model(std::filesystem::path path, ::scene::transform pose, std::uint32_t animation);
    void add_weapon(::weapon::definition definition);
    void init();
    void begin_stream();
    void append_scene(::scene::state patch, bool final = false);
    int run();
    void shutdown() noexcept;
}
