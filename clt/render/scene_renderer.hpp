#pragma once

#include <vk/vk.hpp>

#include <cstdint>

namespace clt::scene_renderer
{
    void create_frame_resources();
    void release_frame_resources();
    
    void init(const char* vertex_path, const char* fragment_path);
    void synchronize();
    void draw(vk::CommandBuffer command, std::uint32_t image);

    void shutdown();
} // namespace clt::scene_renderer
