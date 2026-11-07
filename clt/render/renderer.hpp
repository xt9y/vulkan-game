#pragma once
#include <render/frame.hpp>
#include <render/sequence.hpp>

namespace clt::renderer {
    struct state {
        ::render::frame_scheduler frames;
        ::render::sequence passes;
        bool initialized=false;
    };
    state* get();
    void init(const char* vertex_path,const char* fragment_path);
    void resize();
    void draw_frame();
    void shutdown() noexcept;
}
