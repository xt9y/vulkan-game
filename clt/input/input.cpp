#include "input.hpp"

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
        double last_x = 0;
        double last_y = 0;
    } // namespace

    void init()
    {
        auto* window = clt::window::get()->handle;

        captured = true;
        previous_tab = false;
        previous_escape = false;
        previous_f6 = false;
        previous_f7 = false;

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        if (glfwRawMouseMotionSupported())
        {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }

        glfwGetCursorPos(window, &last_x, &last_y);
    }

    frame poll()
    {
        auto* window = clt::window::get()->handle;

        const auto down = [&](int key)
        {
            return glfwGetKey(window, key) == GLFW_PRESS;
        };

        const bool tab = down(GLFW_KEY_TAB);
        const bool escape = down(GLFW_KEY_ESCAPE);

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

            glfwGetCursorPos(window, &last_x, &last_y);
        }

        double x = 0;
        double y = 0;
        glfwGetCursorPos(window, &x, &y);

        frame result;
        const bool f6 = down(GLFW_KEY_F6);
        const bool f7 = down(GLFW_KEY_F7);
        result.toggle_lighting = f6 && !previous_f6;
        result.toggle_tonemapping = f7 && !previous_f7;
        previous_f6 = f6;
        previous_f7 = f7;

        if (captured && glfwGetWindowAttrib(window, GLFW_FOCUSED))
        {
            result.mouse_x = float(x - last_x);
            result.mouse_y = float(y - last_y);

            if (down(GLFW_KEY_W)) result.buttons |= ::player::forward;
            if (down(GLFW_KEY_S)) result.buttons |= ::player::backward;
            if (down(GLFW_KEY_A)) result.buttons |= ::player::left;
            if (down(GLFW_KEY_D)) result.buttons |= ::player::right;

            if (down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT))
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
