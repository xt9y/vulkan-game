#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <vk/vk.hpp>

namespace clt::renderer
{
    struct frame_context
    {
        vk::CommandBuffer command{};
        vk::Semaphore image_ready{};
        vk::Fence fence{};
    };

    struct state
    {
        std::array<frame_context, 2> frames{};
        std::uint32_t next_frame = 0;
        std::vector<vk::Fence> image_in_flight;
        std::vector<vk::Semaphore> render_ready;
        vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
        bool uncapped = false;
        bool initialized = false;
    };

    state* get();
    void init(const char* vertex_path, const char* fragment_path);
    void resize();
    void draw_frame();
    void shutdown() noexcept;
} // namespace clt::renderer
