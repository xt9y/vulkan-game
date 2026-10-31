#include <cbuild.h>

void build(C_Build* b)
{
    C_Target* game = c_executable(b, "vulkan-game");
    c_sources(game, "main.cpp");
    c_sources(game, "srv/game/*.cpp");
    c_sources(game, "srv/app/*.cpp");
    c_sources(game, "srv/network/*.cpp");
    c_sources(game, "clt/app/*.cpp");
    c_sources(game, "clt/network/*.cpp");
    c_sources(game, "clt/platform/*.cpp");
    c_sources(game, "clt/render/*.cpp");
    c_sources(game, "srv/shared/net/*.cpp");
    c_include(game, ".");
    c_warnings_strict(game);

    c_flag(game, "-std=c++17");

    C_Dependency* engine =
        c_git(b, "VulkanEngine", "https://github.com/xt9y/vulkan-engine.git", "main");
    c_dep_cbuild(engine, "VulkanEngine", C_TARGET_STATIC_LIBRARY);
    c_dep_include(engine, ".");
    c_use(game, engine);

    C_Dependency* headers =
        c_git(b, "VulkanHeaders", "https://github.com/KhronosGroup/Vulkan-Headers.git", "main");
    c_dep_header_only(headers);
    c_dep_include(headers, "include");
    c_use(game, headers);

    C_Dependency* glfw = c_git(b, "GLFW", "https://github.com/glfw/glfw.git", "3.4");
    c_dep_cmake(glfw);
    c_dep_cmake_option(glfw, "-DGLFW_BUILD_EXAMPLES=OFF");
    c_dep_cmake_option(glfw, "-DGLFW_BUILD_TESTS=OFF");
    c_dep_cmake_option(glfw, "-DGLFW_BUILD_DOCS=OFF");
#ifdef __linux__
    c_dep_cmake_option(glfw, "-DGLFW_BUILD_WAYLAND=OFF");
#endif

    c_dep_include(glfw, "include");
    c_dep_link(glfw, "glfw3");
    c_use(game, glfw);

#ifdef __APPLE__
    c_framework(game, "Cocoa");
    c_framework(game, "IOKit");
    c_framework(game, "CoreFoundation");
    c_framework(game, "QuartzCore");
    c_link_system(game, "c++");
#elif defined(__linux__)
    c_link_system(game, "X11");
    c_link_system(game, "Xrandr");
    c_link_system(game, "Xi");
    c_link_system(game, "Xcursor");
    c_link_system(game, "Xinerama");
    c_link_system(game, "dl");
    c_link_system(game, "pthread");
    c_link_system(game, "m");
    c_link_system(game, "stdc++");
#else
    c_link_system(game, "ws2_32");
    c_link_system(game, "user32");
    c_link_system(game, "gdi32");
    c_link_system(game, "shell32");
#endif

    c_default_target(b, game);
}
