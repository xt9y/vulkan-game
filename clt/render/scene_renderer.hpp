#pragma once

#include <vk/vk.hpp>

#include <cstddef>
#include <cstdint>

namespace clt::scene_renderer
{
    struct draw_stats {
        double cull_ms = 0;
        double index_upload_ms = 0;
        double commands_ms = 0;
        std::size_t tested_nodes = 0;
        std::size_t accepted_groups = 0;
        std::size_t visible_instances = 0;
        std::size_t draw_calls = 0;
    };

    draw_stats last_stats();

    void create_frame_resources();
    void release_frame_resources();
    
    void init(const char* vertex_path, const char* fragment_path);
    void synchronize();
    void draw(vk::CommandBuffer command, std::uint32_t image);

    void shutdown();
} // namespace clt::scene_renderer
