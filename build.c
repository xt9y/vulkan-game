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
        "shd/cloud.comp", "shd/gi.comp", "shd/light.comp", "shd/tone.comp",
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

// The original Rogue editor stores each glyph as a tiny PNG instead of a
// font atlas. Sparse checkout fetches only 90 small textures, not the many
// large assets in hellengine-bak. This is optional; fallback glyphs are built
// into VulkanEditor and work without network access.
static void prepare_editor_font(void)
{
    struct stat info;
    if(stat("build/editor-font-repository/Rogue/Rogue/res/textures/font/char_90.png",
            &info)==0)
        return;
    const char* skip=getenv("VULKAN_EDITOR_FONT_SKIP_DOWNLOAD");
    if(skip && strcmp(skip,"1")==0)return;
    if(stat("build/editor-font-repository/.git",&info)!=0)
    {
        if(system("git clone -q --depth 1 --filter=blob:none --sparse "
                  "https://github.com/xt9y/hellengine-bak.git "
                  "build/editor-font-repository")!=0)
        {
            fprintf(stderr,"editor: original font download failed; using fallback\n");
            return;
        }
    }
    // Keep the original pixel-font revision reproducible.
    if(system("git -C build/editor-font-repository fetch -q --depth 1 "
              "origin 2ab96641cc98912ffcdcae67c77a10de5e0fdc7d && "
              "git -C build/editor-font-repository checkout -q --detach "
              "2ab96641cc98912ffcdcae67c77a10de5e0fdc7d && "
              "git -C build/editor-font-repository sparse-checkout set "
              "Rogue/Rogue/res/textures/font")!=0)
        fprintf(stderr,"editor: original font checkout requires GitHub "
                       "access to xt9y/hellengine-bak; using fallback\n");
}

void build(C_Build* b)
{
    compile_shaders();
    prepare_editor_font();
    C_Dependency* engine =
        c_git(b, "VulkanEngine", "https://github.com/xt9y/vulkan-engine.git", "4b792b574727115d7478f414e11e9540e848404e");
    c_dep_cbuild(engine, "VulkanEngine", C_TARGET_STATIC_LIBRARY);
    c_dep_include(engine, ".");

    // Each VulkanSystems module is fetched through its own standalone build.
    // Pin all modules to the same commit. c.lock otherwise retains old main
    // revisions, causing missing headers when modules gain new APIs.
    // Never rely on vulkan-systems/build.c or link a monolithic VulkanSystems.
    C_Dependency* gpu =
        c_git(b, "VulkanGPU", "https://github.com/xt9y/vulkan-systems.git", "7e115b3b6479ef51720aef3c31ef7d27a7213e79");
    c_dep_subdir(gpu, "gpu");
    c_dep_cbuild(gpu, "VulkanGPU", C_TARGET_STATIC_LIBRARY);
    c_dep_include(gpu, ".");

    C_Dependency* render =
        c_git(b, "VulkanRender", "https://github.com/xt9y/vulkan-systems.git", "7e115b3b6479ef51720aef3c31ef7d27a7213e79");
    c_dep_subdir(render, "render");
    c_dep_cbuild(render, "VulkanRender", C_TARGET_STATIC_LIBRARY);
    c_dep_include(render, ".");
    // VulkanRender includes headers from the sibling VulkanGPU module.
    c_dep_include(render, "../gpu");

    C_Dependency* bvh =
        c_git(b, "VulkanBVH", "https://github.com/xt9y/vulkan-systems.git", "7e115b3b6479ef51720aef3c31ef7d27a7213e79");
    c_dep_subdir(bvh, "bvh");
    c_dep_cbuild(bvh, "VulkanBVH", C_TARGET_STATIC_LIBRARY);
    c_dep_include(bvh, ".");

    // The dedicated server needs no GPU, BVH or presentation dependencies.
    C_Dependency* network =
        c_git(b, "VulkanNetwork", "https://github.com/xt9y/vulkan-systems.git", "7e115b3b6479ef51720aef3c31ef7d27a7213e79");
    c_dep_subdir(network, "network");
    c_dep_cbuild(network, "VulkanNetwork", C_TARGET_STATIC_LIBRARY);
    c_dep_include(network, ".");

    C_Dependency* crypto =
        c_git(b, "MbedTLS", "https://github.com/Mbed-TLS/mbedtls.git",
              "068ff080b369adfac81509f9b57b2afabaf82dc5");
    c_dep_cmake(crypto);
    c_dep_cmake_option(crypto, "-DENABLE_TESTING=OFF");
    c_dep_cmake_option(crypto, "-DENABLE_PROGRAMS=OFF");
    c_dep_link(crypto, "mbedtls");
    c_dep_link(crypto, "mbedx509");
    c_dep_link(crypto, "mbedcrypto");

    C_Dependency* spatial =
        c_git(b, "VulkanSpatial", "https://github.com/xt9y/vulkan-systems.git", "7e115b3b6479ef51720aef3c31ef7d27a7213e79");
    c_dep_subdir(spatial, "spatial");
    c_dep_cbuild(spatial, "VulkanSpatial", C_TARGET_STATIC_LIBRARY);
    c_dep_include(spatial, ".");

    C_Dependency* jobs =
        c_git(b, "VulkanJobs", "https://github.com/xt9y/vulkan-systems.git", "7e115b3b6479ef51720aef3c31ef7d27a7213e79");
    c_dep_subdir(jobs, "jobs");
    c_dep_cbuild(jobs, "VulkanJobs", C_TARGET_STATIC_LIBRARY);
    c_dep_include(jobs, ".");

    // Optional, renderer-independent editor: no scene or server dependencies.
    C_Dependency* editor =
        c_git(b, "VulkanEditor", "https://github.com/xt9y/vulkan-systems.git", "3246a3c0beb3b87d5c50bf52df44020cdce92a3d");
    c_dep_subdir(editor, "editor");
    c_dep_cbuild(editor, "VulkanEditor", C_TARGET_STATIC_LIBRARY);
    c_dep_include(editor, ".");

    C_Dependency* headers =
        c_git(b, "VulkanHeaders", "https://github.com/KhronosGroup/Vulkan-Headers.git", "9fbc2a782ee3f8a4db64e7d991e23da75e893f44");
    c_dep_header_only(headers);
    c_dep_include(headers, "include");

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
    c_use(server, network);
    c_use(server, crypto);
    c_use(server, spatial);
    c_use(server, jobs);
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
    // Compile exactly one renderer. Local experimental .cpp files are not linked.
    c_sources(client, "clt/render/renderer.cpp");
    c_sources(client, "clt/render/scene_renderer.cpp");
    c_sources(client, "clt/render/postprocess.cpp");
    c_sources(client, "clt/render/texture.cpp");
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
    // Dependents first, providers after: Render -> GPU -> Engine.
    c_use(client, editor);
    c_use(client, render);
    c_use(client, bvh);
    c_use(client, gpu);
    c_use(client, network);
    c_use(client, crypto);
    c_use(client, spatial);
    c_use(client, jobs);
    c_use(client, engine);
    c_use(client, headers);
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
