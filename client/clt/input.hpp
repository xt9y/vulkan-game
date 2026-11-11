#pragma once
#include "window.hpp"
#include <GLFW/glfw3.h>
#include <array>
#include <cstdint>
#include <utility>

namespace clt::input
{
struct frame {
    std::uint32_t buttons=0;
    float mouse_x=0,mouse_y=0;
};
namespace {double last_x=0,last_y=0;}

void init()
{
    auto* handle=window::get()->handle;
    glfwSetInputMode(handle,GLFW_CURSOR,GLFW_CURSOR_DISABLED);
    if(glfwRawMouseMotionSupported())
        glfwSetInputMode(handle,GLFW_RAW_MOUSE_MOTION,GLFW_TRUE);
    glfwGetCursorPos(handle,&last_x,&last_y);
}
bool key_down(int key)
{
    auto* handle=window::get()->handle;
    return handle && key>=GLFW_KEY_SPACE && key<=GLFW_KEY_LAST &&
        glfwGetWindowAttrib(handle,GLFW_FOCUSED) &&
        glfwGetKey(handle,key)==GLFW_PRESS;
}
bool mouse_down(int button)
{
    auto* handle=window::get()->handle;
    return handle && button>=0 && button<=GLFW_MOUSE_BUTTON_LAST &&
        glfwGetWindowAttrib(handle,GLFW_FOCUSED) &&
        glfwGetMouseButton(handle,button)==GLFW_PRESS;
}
frame poll()
{
    auto* handle=window::get()->handle;
    double x=0,y=0;
    glfwGetCursorPos(handle,&x,&y);
    frame result{};
    if(glfwGetWindowAttrib(handle,GLFW_FOCUSED)) {
        result.mouse_x=float(x-last_x);
        result.mouse_y=float(y-last_y);
        constexpr std::array<std::pair<int,std::uint32_t>,4> movement{{
            {GLFW_KEY_W,::player::forward},{GLFW_KEY_S,::player::backward},
            {GLFW_KEY_A,::player::left},{GLFW_KEY_D,::player::right}
        }};
        for(const auto& [key,flag]:movement)
            if(glfwGetKey(handle,key)==GLFW_PRESS)result.buttons|=flag;
    }
    last_x=x;last_y=y;
    return result;
}
void shutdown()
{
    if(window::get()->handle)
        glfwSetInputMode(window::get()->handle,GLFW_CURSOR,GLFW_CURSOR_NORMAL);
}
} // namespace clt::input
