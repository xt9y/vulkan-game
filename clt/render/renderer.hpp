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

    // Generic game-authored compute/compositing pass. Game-specific
    // simulation code stays in client.cpp, not in the renderer.
    struct effect_callbacks
    {
        void (*create)() = nullptr;
        void (*release)() = nullptr;
        bool (*active)() = nullptr;
        vk::ImageView (*output)(std::uint32_t) = nullptr;
        void (*record)(vk::CommandBuffer, std::uint32_t) = nullptr;
    };

    struct state
    {
        effect_callbacks effect{};
        std::array<frame_context, 2> frames{};
        std::uint32_t next_frame = 0;
        std::vector<vk::Fence> image_in_flight;
        std::vector<vk::Semaphore> render_ready;
        vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
        bool initialized = false;
    };

    state* get();
    void set_effect(effect_callbacks value);
    bool effect_active();
    vk::ImageView effect_output(std::uint32_t image);
    void effect_record(vk::CommandBuffer command, std::uint32_t image);
    void init(const char* vertex_path, const char* fragment_path);
    void resize();
    void draw_frame();
    void shutdown() noexcept;
} // namespace clt::renderer
