#include <cbuild.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static void compile_shaders(void)
{
    static const char* sources[] = {
        "shd/mesh.vert", "shd/mesh.frag", "shd/cull.comp",
        "shd/light.comp", "shd/tone.comp",
        "shd/fullscreen.vert", "shd/present.frag"
    };
#ifdef _WIN32
    _mkdir("build");
    _mkdir("build/shaders");
#else
    mkdir("build", 0755);
    mkdir("build/shaders", 0755);
#endif

    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i)
    {
        char binary[256];
        char command[640];
        const char* name = strrchr(sources[i], '/');
        snprintf(binary, sizeof(binary), "build/shaders/%s.spv",
                 name ? name + 1 : sources[i]);
        struct stat input_info;
        struct stat output_info;
        if (stat(sources[i], &input_info) != 0)
        {
            fprintf(stderr, "Missing shader: %s\n", sources[i]);
            exit(1);
        }
        if (stat(binary, &output_info) == 0 &&
            output_info.st_mtime >= input_info.st_mtime)
        {
            continue;
        }
        snprintf(command, sizeof(command),
                 "glslangValidator -V \"%s\" -o \"%s\"", sources[i], binary);
        if (system(command) != 0)
        {
            fprintf(stderr, "Shader compilation failed: %s\n", sources[i]);
            exit(1);
        }
    }
}

void build(C_Build* b)
{
    compile_shaders();
    C_Target* game = c_executable(b, "vulkan-game");
    c_sources(game, "main.cpp");
    c_sources(game, "srv/world/*.cpp");
    c_sources(game, "srv/scene/*.cpp");
    c_sources(game, "srv/game/*.cpp");
    c_sources(game, "srv/app/*.cpp");
    c_sources(game, "srv/network/*.cpp");
    c_sources(game, "srv/shared/net/*.cpp");
    c_sources(game, "srv/shared/player/*.cpp");

    c_sources(game, "clt/app/*.cpp");
    c_sources(game, "clt/network/*.cpp");
    c_sources(game, "clt/platform/*.cpp");
    c_sources(game, "clt/render/*.cpp");
    c_sources(game, "clt/input/*.cpp");
    c_sources(game, "clt/view/*.cpp");
    c_sources(game, "clt/player/*.cpp");

    c_include(game, ".");
    c_warnings_strict(game);

    c_flag(game, "-std=c++17");

    C_Dependency* engine =
        c_git(b, "VulkanEngine", "https://github.com/xt9y/vulkan-engine.git", "2aa9b9c265995c76859952c6f32b8a75873d22f3");
    c_dep_cbuild(engine, "VulkanEngine", C_TARGET_STATIC_LIBRARY);
    c_dep_include(engine, ".");
    c_use(game, engine);

    C_Dependency* headers =
        c_git(b, "VulkanHeaders", "https://github.com/KhronosGroup/Vulkan-Headers.git", "9fbc2a782ee3f8a4db64e7d991e23da75e893f44");
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

    // Standalone end-to-end benchmark; no changes to the production game.
    C_Target* workload = c_test(b, "workload");
    c_sources(workload, "test.cpp");
    c_sources(workload, "srv/world/*.cpp");
    c_sources(workload, "srv/scene/*.cpp");
    c_sources(workload, "srv/game/*.cpp");
    c_sources(workload, "srv/app/*.cpp");
    c_sources(workload, "srv/network/*.cpp");
    c_sources(workload, "srv/shared/net/*.cpp");
    c_sources(workload, "srv/shared/player/*.cpp");
    c_sources(workload, "clt/app/*.cpp");
    c_sources(workload, "clt/network/*.cpp");
    c_sources(workload, "clt/platform/*.cpp");
    c_sources(workload, "clt/render/*.cpp");
    c_sources(workload, "clt/input/*.cpp");
    c_sources(workload, "clt/view/*.cpp");
    c_sources(workload, "clt/player/*.cpp");
    c_include(workload, ".");
    c_warnings_strict(workload);
    c_flag(workload, "-std=c++17");
    c_use(workload, engine);
    c_use(workload, headers);
    c_use(workload, glfw);
#ifdef __APPLE__
    c_framework(workload, "Cocoa");
    c_framework(workload, "IOKit");
    c_framework(workload, "CoreFoundation");
    c_framework(workload, "QuartzCore");
    c_link_system(workload, "c++");
#elif defined(__linux__)
    c_link_system(workload, "X11");
    c_link_system(workload, "Xrandr");
    c_link_system(workload, "Xi");
    c_link_system(workload, "Xcursor");
    c_link_system(workload, "Xinerama");
    c_link_system(workload, "dl");
    c_link_system(workload, "pthread");
    c_link_system(workload, "m");
    c_link_system(workload, "stdc++");
#else
    c_link_system(workload, "ws2_32");
    c_link_system(workload, "user32");
    c_link_system(workload, "gdi32");
    c_link_system(workload, "shell32");
#endif

    c_default_target(b, game);
}
