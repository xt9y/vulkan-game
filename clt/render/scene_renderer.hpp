#pragma once

#include <vk/vk.hpp>

#include <array>
#include <cstddef>
#include <vector>
#include <cstdint>

namespace clt::scene_renderer
{
    struct culling_mesh {
        const std::vector<std::array<float, 4>>* bounds = nullptr;
        vk::buffer::resource* visible = nullptr;
        vk::buffer::resource* indirect = nullptr;
        std::uint32_t index_count = 0;
    };

    struct culling_readback {
        std::array<vk::util::math::vec4, 6> planes{};
        std::vector<culling_mesh> meshes;
        bool valid = false;
    };

    // Readback views are valid until the next synchronize() or shutdown().
    culling_readback culling_data();

    struct gbuffer {
        // albedo, normal, material, R32_UINT receiver, depth
        std::array<vk::ImageView, 5> views{};
        vk::Extent2D extent{};
    };

    gbuffer outputs(std::uint32_t image);
    vk::util::math::mat4 world_view_projection();

    bool needs_sync();
    void reset_scene();
    void create_frame_resources();
    void release_frame_resources();
    
    void init(const char* vertex_path, const char* fragment_path);
    void synchronize();
    void draw(vk::CommandBuffer command, std::uint32_t image);

    void shutdown();
} // namespace clt::scene_renderer
