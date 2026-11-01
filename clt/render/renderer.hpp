#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <array>
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

    struct diagnostics {
        std::atomic<std::uint64_t> frames{0};
        std::atomic<std::uint64_t> deferred{0};
        std::atomic<std::uint64_t> scene_updates{0};
        std::atomic<std::uint64_t> fence_wait_us{0};
        std::atomic<std::uint64_t> max_fence_wait_us{0};
        std::atomic<std::uint64_t> max_frame_us{0};
    };

    diagnostics& stats();
    state* get();
    void init(const char* vertex_path, const char* fragment_path);
    void resize();
    void draw_frame();
    void shutdown() noexcept;
} // namespace clt::renderer
