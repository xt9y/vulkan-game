#include <cbuild.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#ifndef _WIN32
#define WRAPPER_PYTHON "python3"
#else
#define WRAPPER_PYTHON "python"
#endif

// The Vulkan libraries are checked-in Git submodules. No duplicate Git
// checkouts, dependency cache or remote Vulkan cbuild invocation is needed.
static void prepare_vulkan_submodules(void)
{
    static const char* required[]={
        "third-party/vulkan-wrapper/build.c",
        "third-party/vulkan-systems/gpu/build.c",
        "third-party/vulkan-headers/registry/vk.xml"
    };
    int missing=0;
    for(size_t i=0;i<sizeof(required)/sizeof(required[0]);++i)
    {
        struct stat info;
        if(stat(required[i],&info)!=0){missing=1;break;}
    }
    if(missing && system("git submodule update --init --recursive")!=0)
    {
        fprintf(stderr,"Failed to initialize Vulkan Git submodules\n");
        exit(1);
    }
    for(size_t i=0;i<sizeof(required)/sizeof(required[0]);++i)
    {
        struct stat info;
        if(stat(required[i],&info)!=0)
        {
            fprintf(stderr,"Missing Vulkan submodule file: %s\n",required[i]);
            exit(1);
        }
    }
}

static void local_vulkan_includes(C_Target* target)
{
    c_include(target,"third-party/vulkan-wrapper");
    c_include(target,"third-party/vulkan-wrapper/vk");
    c_include(target,"third-party/vulkan-headers/include");
    c_include(target,"build/generated");
    c_include(target,"third-party/vulkan-systems/gpu");
    c_include(target,"third-party/vulkan-systems/render");
    c_include(target,"third-party/vulkan-systems/bvh");
    c_include(target,"third-party/vulkan-systems/network");
    c_include(target,"third-party/vulkan-systems/spatial");
    c_include(target,"third-party/vulkan-systems/jobs");
    c_include(target,"third-party/vulkan-systems/editor");
}

static C_Target* local_vulkan_wrapper(C_Build* b)
{
    C_Target* target=c_static_library(b,"VulkanWrapper");
    c_sources(target,"third-party/vulkan-wrapper/vk/*/*.cpp");
    c_generate(target,"build/generated/generated.cpp",
        "third-party/vulkan-headers/registry/vk.xml",
        WRAPPER_PYTHON " third-party/vulkan-wrapper/vk/tools/generate.py "
        "third-party/vulkan-headers/registry/vk.xml "
        "build/generated/generated.hpp build/generated/generated.cpp "
        "--headers third-party/vulkan-headers/include");
    local_vulkan_includes(target);
    c_flag(target,"-std=c++17");
#ifdef __linux__
    c_link_system(target,"dl");
#endif
    return target;
}

static C_Target* local_vulkan_system(C_Build* b,const char* name,
                                      const char* folder,C_Target* wrapper)
{
    C_Target* target=c_static_library(b,name);
    char source[256];
    snprintf(source,sizeof(source),
             "third-party/vulkan-systems/%s/%s/*.cpp",folder,folder);
    c_sources(target,source);
    local_vulkan_includes(target);
    c_flag(target,"-std=c++17");
    if(wrapper)c_link_target(target,wrapper);
    return target;
}


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

// Original menu textures remain from Hell2024 (7362a3a); the font
// is embedded directly from the user's x_font.png in VulkanEditor.
// No legacy Hell2024 font files are needed.
static void prepare_editor_assets(void)
{
    struct stat menu;
    const char* menu_path=
        "build/editor-hell2024/Hell2024/Hell2024/res/textures/ui/MenuBG.png";
    if(stat(menu_path,&menu)==0)return;
    if(stat("build/editor-hell2024/.git",&menu)!=0)
    {
        if(system("git init -q build/editor-hell2024 && "
                  "git -C build/editor-hell2024 remote add origin "
                  "https://github.com/livinamuk/Hell2025.git && "
                  "git -C build/editor-hell2024 sparse-checkout init --cone")!=0)
        {
            fprintf(stderr,"editor: could not initialize original UI assets\n");
            return;
        }
    }
    if(system("git -C build/editor-hell2024 sparse-checkout set "
              "Hell2024/Hell2024/res/textures/ui && "
              "git -C build/editor-hell2024 fetch -q --depth 1 "
              "--filter=blob:none origin "
              "7362a3a86cc254ecffdf498c9febae10eefa120b && "
              "git -C build/editor-hell2024 checkout -q --detach FETCH_HEAD")!=0)
    {
        fprintf(stderr,
                "editor: could not fetch exact Hell2024 UI snapshot\n");
        return;
    }
    if(stat(menu_path,&menu)!=0)
        fprintf(stderr,"editor: original menu textures incomplete\n");
}

void build(C_Build* b)
{
    prepare_vulkan_submodules();
    compile_shaders();
    prepare_editor_assets();
    C_Target* wrapper=local_vulkan_wrapper(b);
    C_Target* gpu=local_vulkan_system(b,"VulkanGPU","gpu",wrapper);
    C_Target* render=local_vulkan_system(b,"VulkanRender","render",wrapper);
    c_link_target(render,gpu);
    C_Target* bvh=local_vulkan_system(b,"VulkanBVH","bvh",wrapper);
    C_Target* network=local_vulkan_system(b,"VulkanNetwork","network",NULL);

    C_Dependency* crypto =
        c_git(b, "MbedTLS", "https://github.com/Mbed-TLS/mbedtls.git",
              "068ff080b369adfac81509f9b57b2afabaf82dc5");
    c_dep_cmake(crypto);
    c_dep_cmake_option(crypto, "-DENABLE_TESTING=OFF");
    c_dep_cmake_option(crypto, "-DENABLE_PROGRAMS=OFF");
    c_dep_link(crypto, "mbedtls");
    c_dep_link(crypto, "mbedx509");
    c_dep_link(crypto, "mbedcrypto");

    C_Target* spatial=local_vulkan_system(b,"VulkanSpatial","spatial",wrapper);
    C_Target* jobs=local_vulkan_system(b,"VulkanJobs","jobs",NULL);
#ifndef _WIN32
    c_link_system(jobs,"pthread");
#endif
    C_Target* editor=local_vulkan_system(b,"VulkanEditor","editor",wrapper);
    c_use(network,crypto); // mbedTLS is required when compiling network/*.cpp.

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
    c_use(editor,stb); // STB_IMAGE_STATIC bitmap decoder in VulkanEditor.

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
    local_vulkan_includes(server);
    c_warnings_strict(server);
    c_flag(server, "-std=c++20");
    c_link_target(server, network);
    c_use(server, crypto);
    c_link_target(server, spatial);
    c_link_target(server, jobs);
    c_link_target(server, wrapper);
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
    local_vulkan_includes(client);
    c_warnings_strict(client);
    c_flag(client, "-std=c++20");
    // Dependents first, providers after: Render -> GPU -> Wrapper.
    c_link_target(client, editor);
    c_link_target(client, render);
    c_link_target(client, bvh);
    c_link_target(client, gpu);
    c_link_target(client, network);
    c_use(client, crypto);
    c_link_target(client, spatial);
    c_link_target(client, jobs);
    c_link_target(client, wrapper);
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
