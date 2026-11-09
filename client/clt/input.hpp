#pragma once
#include "window.hpp"
#include <GLFW/glfw3.h>
#include <cstdint>

namespace clt::input {
    struct frame {
        std::uint32_t buttons = 0;
        float mouse_x = 0;
        float mouse_y = 0;
    };
    void init();
    bool key_down(int key);
    bool mouse_down(int button);
    frame poll();
    void shutdown();

namespace {
        double last_x = 0;
        double last_y = 0;
    }
    void init() {
        auto* window = clt::window::get()->handle;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        glfwGetCursorPos(window, &last_x, &last_y);
    }
    bool key_down(int key) {
        auto* window = clt::window::get()->handle;
return window && key >= GLFW_KEY_SPACE && key <= GLFW_KEY_LAST && glfwGetWindowAttrib(window, GLFW_FOCUSED) && glfwGetKey(window, key) == GLFW_PRESS;
    }
    bool mouse_down(int button) {
        auto* window = clt::window::get()->handle;
return window && button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST && glfwGetWindowAttrib(window, GLFW_FOCUSED) && glfwGetMouseButton(window, button) == GLFW_PRESS;
    }
    frame poll() {
        auto* window = clt::window::get()->handle;
        double x, y;
        glfwGetCursorPos(window, &x, &y);
        frame result{};
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
            result.mouse_x = float(x - last_x);
            result.mouse_y = float(y - last_y);
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                result.buttons |= ::player::forward;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                result.buttons |= ::player::backward;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                result.buttons |= ::player::left;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                result.buttons |= ::player::right;
        }
        last_x = x;
        last_y = y;
        return result;
    }
    void shutdown() {
        if (clt::window::get()->handle)
            glfwSetInputMode(clt::window::get()->handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

} // namespace clt::input
