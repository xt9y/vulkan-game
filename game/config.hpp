#pragma once
#include "srv/game/options.hpp"

// Game-specific values belong here, not in the reusable engine.
// The server and client use the same movement and simulation frequency.
namespace game::config
{
    inline options::state settings()
    {
        options::state s{};
        s.host = "127.0.0.1";
        s.bind_host = "127.0.0.1";
        s.port = 27015;
        s.duration = 0;
        s.headless = false;
        s.help = false;

        s.player.spawn = {0, 0, 4};
        s.player.walk_speed = 5.0f;
        s.player.sprint_speed = 8.0f;
        s.player.gravity = 20.0f;
        s.player.radius = 0.3f;
        s.player.height = 1.8f;

        s.view.fov = 1.04719755f;
        s.view.sensitivity = 0.0025f;
        s.view.eye_height = 1.65f;
        s.view.pitch_limit = 1.55f;
        s.view.near_plane = 0.1f;
        s.view.far_plane = 1000.0f;

        s.simulation.tick_hz = 60;
        s.simulation.max_clients = 64;
        s.simulation.timeout_seconds = 5;
        s.simulation.receive_packets = 96;
        s.simulation.receive_ms = 2;
        s.simulation.fragments_per_tick = 64;
        s.simulation.fragments_ms = 2;
        s.simulation.inputs_per_tick = 4;
        s.simulation.animation_period = 6;
        s.simulation.animations_per_tick = 64;
        s.simulation.generation_slack_ms = 2;
        s.simulation.generation_commit_ticks = 8;
        s.simulation.world_seed = 123456789ULL;
        s.simulation.terrain_base = 16;
        s.simulation.terrain_scale = 64;
        s.simulation.terrain_amplitude = 10;
        s.simulation.detail_scale = 16;
        s.simulation.detail_amplitude = 3;

        s.graphics.width = 1280;
        s.graphics.height = 720;
        s.graphics.title = "vulkan-game";
        s.graphics.vertex_shader = "build/shaders/mesh.vert.spv";
        s.graphics.fragment_shader = "build/shaders/mesh.frag.spv";
        s.graphics.uncapped = false;
        s.graphics.lighting = true;
        s.graphics.tonemapping = true;
        s.graphics.backface_culling = false;
        s.graphics.options_visible = false;
        s.graphics.options_selected = 0;
        s.graphics.exposure = 1.0f;
        s.graphics.correction_limit = 2.0f;
        s.graphics.correction_decay = 0.8f;
        s.graphics.exposure_step = 0.1f;
        s.graphics.fov_step = 0.05f;
        s.graphics.sensitivity_step = 0.00025f;
        s.graphics.min_exposure = 0.0f;
        s.graphics.max_exposure = 5.0f;
        s.graphics.min_fov = 0.5f;
        s.graphics.max_fov = 2.2f;
        s.graphics.min_sensitivity = 0.00025f;
        s.graphics.max_sensitivity = 0.02f;
        s.graphics.timeout_ms = 3000;
        s.graphics.retry_ms = 250;
        s.graphics.retry_max_ms = 1000;
        s.graphics.heartbeat_ms = 50;
        s.graphics.scene_request_ms = 40;
        s.graphics.movement_ms = 16;
        return s;
    }
} // namespace game::config
