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
        double gpu_dispatch_ms = 0;
        bool gpu_culling = false;
        std::size_t gpu_candidates = 0;
        std::size_t tested_nodes = 0;
        std::size_t accepted_groups = 0;
        std::size_t cache_hits = 0;
        std::size_t uploaded_bytes = 0;
        std::size_t visible_instances = 0;
        std::size_t draw_calls = 0;
    };

    draw_stats last_stats();

    struct gpu_validation {
        bool available = false;
        std::size_t expected = 0;
        std::size_t actual = 0;
        std::size_t missing = 0;
        std::size_t unexpected = 0;
        std::size_t duplicates = 0;
        std::size_t out_of_range = 0;
        std::size_t bad_commands = 0;

        bool passed() const
        {
            return available && expected == actual && !missing &&
                !unexpected && !duplicates && !out_of_range && !bad_commands;
        }
    };

    // Call only after waiting for the frame fence. Checks every GPU-produced
    // instance ID against the exact frustum used by the compute shader.
    gpu_validation validate_gpu_visibility();

    void create_frame_resources();
    void release_frame_resources();
    
    void init(const char* vertex_path, const char* fragment_path);
    void synchronize();
    void draw(vk::CommandBuffer command, std::uint32_t image);

    void shutdown();
} // namespace clt::scene_renderer
