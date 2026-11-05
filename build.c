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
    C_Dependency* engine =
        c_git(b, "VulkanEngine", "https://github.com/xt9y/vulkan-engine.git", "2aa9b9c265995c76859952c6f32b8a75873d22f3");
    c_dep_cbuild(engine, "VulkanEngine", C_TARGET_STATIC_LIBRARY);
    c_dep_include(engine, ".");

    C_Dependency* headers =
        c_git(b, "VulkanHeaders", "https://github.com/KhronosGroup/Vulkan-Headers.git", "9fbc2a782ee3f8a4db64e7d991e23da75e893f44");
    c_dep_header_only(headers);
    c_dep_include(headers, "include");

    C_Dependency* fluid =
        c_git(b, "VulkanFluid", "https://github.com/xt9y/vulkan-systems.git",
              "e71b02cb369fc52fdda18cde569318c2904fd708");
    c_dep_subdir(fluid, "fluid");
    c_dep_cbuild(fluid, "VulkanFluid", C_TARGET_STATIC_LIBRARY);
    c_dep_include(fluid, ".");

    C_Dependency* fastgltf =
        c_git(b, "fastgltf", "https://github.com/spnda/fastgltf.git", "0b800b4a584704f9e9add67c79c4c8732e552787");
    c_dep_cmake(fastgltf);
    c_dep_cmake_option(fastgltf, "-DBUILD_SHARED_LIBS=OFF");
    c_dep_cmake_option(fastgltf, "-DFASTGLTF_ENABLE_INSTALL=ON");
    c_dep_cmake_option(fastgltf, "-DCMAKE_DISABLE_FIND_PACKAGE_simdjson=ON");
    c_dep_cmake_option(fastgltf, "-DCMAKE_DISABLE_FIND_PACKAGE_simdutf=ON");
    c_dep_include(fastgltf, "include");
    c_dep_link(fastgltf, "fastgltf");
    c_dep_link(fastgltf, "fastgltf_simdjson");
    c_dep_link(fastgltf, "fastgltf_simdutf");

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

    C_Dependency* stb =
        c_git(b, "stb", "https://github.com/nothings/stb.git", "d14d46f6b44456eb88beac58cba833a4135d4c89");
    c_dep_header_only(stb);
    c_dep_include(stb, ".");

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

    // Dedicated server: no window system or graphics dependency.
    C_Target* server = c_executable(b, "vulkan-server");
    c_sources(server, "server.cpp");
    c_sources(server, "weapons.cpp");
    c_sources(server, "srv/jobs/worker.cpp");
    c_sources(server, "srv/assets/*.cpp");
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
    c_sources(client, "weapons.cpp");
    c_sources(client, "clt/app/*.cpp");
    c_sources(client, "clt/network/*.cpp");
    c_sources(client, "clt/platform/*.cpp");
    c_sources(client, "clt/render/*.cpp");
    c_sources(client, "clt/animation/*.cpp");
    c_sources(client, "clt/input/*.cpp");
    c_sources(client, "clt/view/*.cpp");
    c_sources(client, "clt/player/*.cpp");
    c_sources(client, "clt/weapon/*.cpp");
    c_sources(client, "srv/assets/*.cpp");
    c_sources(client, "srv/shared/net/*.cpp");
    c_sources(client, "srv/shared/player/*.cpp");
    c_sources(client, "srv/game/options.cpp");
    c_sources(client, "srv/game/state.cpp");
    c_include(client, ".");
    c_warnings_strict(client);
    c_flag(client, "-std=c++20");
    c_use(client, engine);
    c_use(client, fluid);
    c_use(client, headers);
    // Grid and volume-surface shaders come from the shared fluid library.
    // No SPH/BVH shader variants are compiled into the game.
#define CONT_GRID(NAME) \
    c_generate(client, "build/shaders/continuum_" #NAME "_stamp.cpp", \
               "@VulkanFluid@/fld/continuum/shaders/grid.comp", \
               "glslangValidator -V -DCONT_" #NAME \
               " @VulkanFluid@/fld/continuum/shaders/grid.comp" \
               " -o build/shaders/continuum_" #NAME ".comp.spv && " \
               "printf 'namespace continuum_" #NAME "_stamp {}\\n'" \
               " > build/shaders/continuum_" #NAME "_stamp.cpp")
    CONT_GRID(SEED);
    CONT_GRID(ADVECT_VELOCITY);
    CONT_GRID(CURVATURE);
    CONT_GRID(DIVERGENCE);
    CONT_GRID(PRESSURE);
    CONT_GRID(PROJECT);
    CONT_GRID(ADVECT_VOLUME);
#undef CONT_GRID
#define CONT_RENDER(NAME,DEFINE,EXTRA) \
    c_generate(client, "build/shaders/continuum_" #NAME "_stamp.cpp", \
               "@VulkanFluid@/fld/continuum/shaders/surface.comp", \
               "glslangValidator -V -DCONT_" #DEFINE " " EXTRA \
               " @VulkanFluid@/fld/continuum/shaders/surface.comp" \
               " -o build/shaders/continuum_" #NAME ".comp.spv && " \
               "printf 'namespace continuum_" #NAME "_stamp {}\\n'" \
               " > build/shaders/continuum_" #NAME "_stamp.cpp")
    // Previous clear-water surface stage intentionally disabled for the
    // blood demo. Both materials still reuse the same VOF/MAC solver.
    // CONT_RENDER(surface,RENDER,"");
    CONT_RENDER(blood_surface,RENDER,"-DFLD_BLOOD");
    CONT_RENDER(upscale,UPSCALE,"-DFLD_BLOOD");
#undef CONT_RENDER
    c_use(client, glfw);
    c_use(client, fastgltf);
    c_use(client, ozz);
    c_use(client, stb);

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

    c_default_target(b, server);
}
