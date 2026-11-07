#pragma once

#include "util/math/vector.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace game::options
{
    enum class mode { server_only, client_only };
    enum class gbuffer_precision { unspecified, rgba8, rgba16f };
    enum class surface_format { unspecified, bgra8_srgb, rgba8_srgb };
    enum class depth_precision { unspecified, d32, d16 };
    enum class presentation { unspecified, fifo, mailbox, immediate };

    struct point_light
    {
        vk::util::math::vec3 position;
        float range;
        vk::util::math::vec3 color;
        float intensity;
    };

    struct client_graphics
    {
        int width, height;
        std::string title, vertex_shader, fragment_shader;
        gbuffer_precision gbuffer;
        surface_format surface;
        depth_precision depth;
        presentation present;
        std::string culling_shader, lighting_shader, tonemap_shader;
        std::string fullscreen_shader, presentation_shader;
        float exposure;
        vk::util::math::vec3 sun_direction;
        float sun_intensity;
        std::vector<point_light> point_lights;
    };

    struct state
    {
        options::mode mode;
        std::string host, bind_host;
        std::uint16_t port;
        double duration;
        bool headless, help;
        client_graphics graphics;
    };

    state* get();
    void init(int argc, char** argv, mode selected_mode, state configured);
    void print_help();
    void shutdown();
} // namespace game::options
