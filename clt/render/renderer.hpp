#pragma once

#include <vector>
#include <vk/vk.hpp>

namespace clt::renderer
{
    struct state
    {
        vk::CommandBuffer command{};
        vk::Semaphore image_ready{};
        vk::Fence frame_fence{};
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
