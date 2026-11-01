#pragma once

#include <vk/vk.hpp>

#include <cstddef>
#include <cstdint>

namespace clt::scene_renderer
{
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
