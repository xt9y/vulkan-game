#include "input.hpp"
#include "srv/game/options.hpp"

#include "clt/platform/window.hpp"
#include "srv/shared/player/movement.hpp"

#include <GLFW/glfw3.h>

namespace clt::input
{
    namespace {
        bool captured = true;
        bool previous_tab = false;
        bool previous_escape = false;
        bool previous_f6 = false;
        bool previous_f7 = false;
        bool previous_overlay = false;
        bool previous_up = false;
        bool previous_down = false;
        bool previous_decrease = false;
        bool previous_increase = false;
        double last_x = 0;
        double last_y = 0;
    } // namespace

    void init()
    {
        auto* window = clt::window::get()->handle;

        captured = game::options::get()->controls.captured_on_start;
        previous_tab = false;
        previous_escape = false;
        previous_f6 = false;
        previous_f7 = false;
        previous_overlay = previous_up = previous_down = false;
        previous_decrease = previous_increase = false;

        glfwSetInputMode(window, GLFW_CURSOR,
                         captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

        if (captured && glfwRawMouseMotionSupported())
        {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }

        glfwGetCursorPos(window, &last_x, &last_y);
    }

    frame poll()
    {
        auto* window = clt::window::get()->handle;
        const auto& cfg = game::options::get()->controls;

        const auto down = [&](int key)
        {
            return glfwGetKey(window, key) == GLFW_PRESS;
        };

        const bool tab = down(cfg.capture);
        const bool escape = down(cfg.release);

        const bool toggle = tab && !previous_tab;
        const bool release = escape && !previous_escape;

        previous_tab = tab;
        previous_escape = escape;

        if (toggle || release)
        {
            captured = toggle ? !captured : false;

            glfwSetInputMode(
                window,
                GLFW_CURSOR,
                captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
            );

            if (glfwRawMouseMotionSupported())
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
            glfwGetCursorPos(window, &last_x, &last_y);
        }

        double x = 0;
        double y = 0;
        glfwGetCursorPos(window, &x, &y);

        frame result;
        const bool f6 = down(cfg.lighting);
        const bool f7 = down(cfg.tonemapping);
        result.toggle_lighting = f6 && !previous_f6;
        result.toggle_tonemapping = f7 && !previous_f7;
        previous_f6 = f6;
        previous_f7 = f7;

        const bool overlay = down(cfg.overlay);
        const bool up = down(cfg.up);
        const bool next = down(cfg.down);
        const bool decrease = down(cfg.decrease);
        const bool increase = down(cfg.increase);
        result.toggle_options = overlay && !previous_overlay;
        result.options_up = up && !previous_up;
        result.options_down = next && !previous_down;
        result.options_decrease = decrease && !previous_decrease;
        result.options_increase = increase && !previous_increase;
        previous_overlay = overlay;
        previous_up = up;
        previous_down = next;
        previous_decrease = decrease;
        previous_increase = increase;

        if (captured && glfwGetWindowAttrib(window, GLFW_FOCUSED))
        {
            result.mouse_x = float(x - last_x);
            result.mouse_y = float(y - last_y);

            if (down(cfg.forward)) result.buttons |= ::player::forward;
            if (down(cfg.backward)) result.buttons |= ::player::backward;
            if (down(cfg.left)) result.buttons |= ::player::left;
            if (down(cfg.right)) result.buttons |= ::player::right;

            if (down(cfg.sprint_left) || down(cfg.sprint_right))
            {
                result.buttons |= ::player::sprint;
            }
        }

        last_x = x;
        last_y = y;

        return result;
    }

    void shutdown()
    {
        if (clt::window::get()->handle)
        {
            glfwSetInputMode(
                clt::window::get()->handle,
                GLFW_CURSOR,
                GLFW_CURSOR_NORMAL
            );
        }
    }
} // namespace clt::input
