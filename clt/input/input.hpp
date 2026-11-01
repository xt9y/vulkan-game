#pragma once

#include <cstdint>

namespace clt::input
{
    struct frame {
        std::uint32_t buttons = 0;
        float mouse_x = 0;
        float mouse_y = 0;
    };

    void init();
    frame poll();
    void shutdown();
} // namespace clt::input
