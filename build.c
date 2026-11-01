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
    c_sources(game, "srv/assets/*.cpp");
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

    c_flag(game, "-std=c++20");

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

    C_Dependency* fastgltf =
        c_git(b, "fastgltf", "https://github.com/spnda/fastgltf.git", "0b800b4a584704f9e9add67c79c4c8732e552787");
    c_dep_cmake(fastgltf);
    c_dep_cmake_option(fastgltf, "-DBUILD_SHARED_LIBS=OFF");
    c_dep_cmake_option(fastgltf, "-DFASTGLTF_ENABLE_INSTALL=OFF");
    c_dep_cmake_option(fastgltf, "-DCMAKE_DISABLE_FIND_PACKAGE_simdjson=ON");
    c_dep_cmake_option(fastgltf, "-DCMAKE_DISABLE_FIND_PACKAGE_simdutf=ON");
    c_dep_include(fastgltf, "include");
    c_dep_link(fastgltf, "fastgltf");
    c_dep_link(fastgltf, "fastgltf_simdjson");
    c_dep_link(fastgltf, "fastgltf_simdutf");
    c_use(game, fastgltf);

    C_Dependency* ozz =
        c_git(b, "ozz", "https://github.com/guillaumeblanc/ozz-animation.git", "744eb9d99f606eda849acb0b1204f7a3dc20bca1");
    c_dep_cmake(ozz);
    c_dep_cmake_option(ozz, "-DBUILD_SHARED_LIBS=OFF");
    c_dep_cmake_option(ozz, "-Dozz_build_tools=OFF");
    c_dep_cmake_option(ozz, "-Dozz_build_samples=OFF");
    c_dep_cmake_option(ozz, "-Dozz_build_howtos=OFF");
    c_dep_cmake_option(ozz, "-Dozz_build_tests=OFF");
    c_dep_cmake_option(ozz, "-Dozz_build_postfix=OFF");
    c_dep_include(ozz, "include");
    c_dep_link(ozz, "ozz_animation_offline");
    c_dep_link(ozz, "ozz_animation");
    c_dep_link(ozz, "ozz_geometry");
    c_dep_link(ozz, "ozz_base");
    c_use(game, ozz);

    C_Dependency* stb =
        c_git(b, "stb", "https://github.com/nothings/stb.git", "d14d46f6b44456eb88beac58cba833a4135d4c89");
    c_dep_header_only(stb);
    c_dep_include(stb, ".");
    c_use(game, stb);

    C_Dependency* mikktspace =
        c_git(b, "MikkTSpace", "https://github.com/mmikk/MikkTSpace.git", "3e895b49d05ea07e4c2133156cfa94369e19e409");
    c_dep_source(mikktspace);
    c_dep_sources(mikktspace, "mikktspace.c");
    c_dep_include(mikktspace, ".");
    c_use(game, mikktspace);

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

    // Dedicated server: no window system or graphics dependency.
    C_Target* server = c_executable(b, "vulkan-server");
    c_sources(server, "server.cpp");
    c_sources(server, "srv/world/*.cpp");
    c_sources(server, "srv/assets/*.cpp");
    c_sources(server, "srv/scene/*.cpp");
    c_sources(server, "srv/app/*.cpp");
    c_sources(server, "srv/network/*.cpp");
    c_sources(server, "srv/shared/net/*.cpp");
    c_sources(server, "srv/shared/player/*.cpp");
    c_sources(server, "srv/game/options.cpp");
    c_sources(server, "srv/game/state.cpp");
    c_include(server, ".");
    c_warnings_strict(server);
    c_flag(server, "-std=c++20");
    c_use(server, engine);
    c_use(server, headers);
    c_use(server, fastgltf);
    c_use(server, ozz);
    c_use(server, stb);

    // Dedicated client: no server world generation or server application.
    C_Target* client = c_executable(b, "vulkan-client");
    c_sources(client, "client.cpp");
    c_sources(client, "clt/app/*.cpp");
    c_sources(client, "clt/network/*.cpp");
    c_sources(client, "clt/platform/*.cpp");
    c_sources(client, "clt/render/*.cpp");
    c_sources(client, "clt/input/*.cpp");
    c_sources(client, "clt/view/*.cpp");
    c_sources(client, "clt/player/*.cpp");
    c_sources(client, "srv/shared/net/*.cpp");
    c_sources(client, "srv/shared/player/*.cpp");
    c_sources(client, "srv/game/options.cpp");
    c_sources(client, "srv/game/state.cpp");
    c_include(client, ".");
    c_warnings_strict(client);
    c_flag(client, "-std=c++20");
    c_use(client, engine);
    c_use(client, headers);
    c_use(client, glfw);
    c_use(client, ozz);

#ifdef __APPLE__
    c_link_system(server, "c++");
    c_framework(client, "Cocoa");
    c_framework(client, "IOKit");
    c_framework(client, "CoreFoundation");
    c_framework(client, "QuartzCore");
    c_link_system(client, "c++");
#elif defined(__linux__)
    c_link_system(server, "pthread");
    c_link_system(server, "m");
    c_link_system(server, "stdc++");
    c_link_system(client, "X11");
    c_link_system(client, "Xrandr");
    c_link_system(client, "Xi");
    c_link_system(client, "Xcursor");
    c_link_system(client, "Xinerama");
    c_link_system(client, "dl");
    c_link_system(client, "pthread");
    c_link_system(client, "m");
    c_link_system(client, "stdc++");
#else
    c_link_system(server, "ws2_32");
    c_link_system(client, "ws2_32");
    c_link_system(client, "user32");
    c_link_system(client, "gdi32");
    c_link_system(client, "shell32");
#endif

    // Standalone end-to-end benchmark; no changes to the production game.
    C_Target* workload = c_test(b, "workload");
    c_sources(workload, "test.cpp");
    c_sources(workload, "srv/world/*.cpp");
    c_sources(workload, "srv/assets/*.cpp");
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
    c_flag(workload, "-std=c++20");
    c_use(workload, engine);
    c_use(workload, headers);
    c_use(workload, glfw);
    c_use(workload, fastgltf);
    c_use(workload, ozz);
    c_use(workload, stb);
    c_use(workload, mikktspace);
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
