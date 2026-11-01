#pragma once

#include <cstdint>

namespace clt::input
{
    struct frame {
        std::uint32_t buttons = 0;
        float mouse_x = 0;
        float mouse_y = 0;
        bool toggle_lighting = false;
        bool toggle_tonemapping = false;
        bool toggle_options = false;
        bool options_up = false;
        bool options_down = false;
        bool options_decrease = false;
        bool options_increase = false;
    };

    void init();
    frame poll();
    void shutdown();
} // namespace clt::input
