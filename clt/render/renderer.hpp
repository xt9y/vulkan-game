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
        bool initialized = false;
    };

    state* get();
    void init();
    void resize();
    void draw_frame();
    void shutdown() noexcept;
} // namespace clt::renderer
