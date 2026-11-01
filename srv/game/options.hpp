#pragma once
#include "util/math/vector.hpp"
#include <cstdint>
#include <string>

namespace game::options
{
    enum class mode { server_only, client_only };

    struct movement
    {
        vk::util::math::vec3 spawn;
        float walk_speed, sprint_speed, gravity, radius, height;
    };

    struct camera
    {
        float fov, sensitivity, eye_height, pitch_limit, near_plane, far_plane;
    };

    struct input
    {
        int forward, backward, left, right, sprint_left, sprint_right;
        int capture, release, lighting, tonemapping;
        bool captured_on_start;
        int overlay, up, down, decrease, increase;
    };

    struct server
    {
        std::uint32_t tick_hz, max_clients, timeout_seconds;
        std::uint32_t receive_packets, receive_ms, fragments_per_tick;
        std::uint32_t fragments_ms, inputs_per_tick, animation_period, animations_per_tick;
        std::uint32_t generation_slack_ms, generation_commit_ticks;
        std::uint64_t world_seed;
        std::int32_t terrain_base, terrain_scale, terrain_amplitude;
        std::int32_t detail_scale, detail_amplitude;
    };

    struct client
    {
        int width, height;
        std::string title, vertex_shader, fragment_shader;
        bool uncapped, lighting, tonemapping, backface_culling;
        bool options_visible;
        int options_selected;
        float exposure, correction_limit, correction_decay;
        float exposure_step, fov_step, sensitivity_step;
        float min_exposure, max_exposure, min_fov, max_fov;
        float min_sensitivity, max_sensitivity;
        std::uint32_t timeout_ms, retry_ms, retry_max_ms, heartbeat_ms, scene_request_ms, movement_ms;
    };

    struct state
    {
        options::mode mode;
        std::string host, bind_host;
        std::uint16_t port;
        double duration;
        bool headless, help;
        movement player;
        camera view;
        input controls;
        server simulation;
        client graphics;
    };

    state* get();
    void init(int argc, char** argv, mode selected_mode, state configured);
    void print_help();
    void shutdown();
} // namespace game::options
