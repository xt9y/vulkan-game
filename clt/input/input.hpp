#pragma once
#include <cstdint>
namespace clt::input
{
    struct frame
    {
        std::uint32_t buttons = 0;
        float mouse_x = 0;
        float mouse_y = 0;
    };
    void init();
    // GLFW key/button identifiers; query on the main thread. Unfocused input is released.
    bool key_down(int key);
    bool mouse_down(int button);
    frame poll();
    void shutdown();
} // namespace clt::input
