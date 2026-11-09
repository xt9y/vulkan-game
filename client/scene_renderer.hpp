#pragma once
#include "camera.hpp"
#include <pcloud/pcloud.hpp>
#include <vk/vk.hpp>
#include <cstdint>

namespace clt::scene_renderer {
    pc::gbuffer outputs(std::uint32_t image);
    bool needs_sync();
    void create_frame_resources();
    void release_frame_resources();
    void init(const char* vertex_path, const char* fragment_path);
    void synchronize();
void draw(vk::CommandBuffer command, std::uint32_t image, const camera::frame& camera);
    void shutdown();

} // namespace clt::scene_renderer
