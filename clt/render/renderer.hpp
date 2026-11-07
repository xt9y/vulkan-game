#pragma once
#include <render/frame.hpp>
#include <render/sequence.hpp>
#include <gpu/timestamps.hpp>
#include <array>
#include <cstdint>

namespace clt::renderer {
    struct state {
        ::render::frame_scheduler frames;
        ::render::sequence passes;
        gpu::timestamps timings;
        std::array<double,6> gpu_ms{};
        std::uint32_t profile_frames=0;
        bool timestamps_supported=false;
        bool initialized=false;
    };
    state* get();
    void init(const char* vertex_path,const char* fragment_path);
    void resize();
    void draw_frame();
    void shutdown() noexcept;
}
