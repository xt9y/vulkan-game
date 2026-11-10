#include <cbuild.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "third-party/vulkan-systems/pcloud/integration.h"

#ifdef _WIN32
#define PYTHON "python"
#else
#define PYTHON "python3"
#endif

static const pc_build_paths paths={
    "third-party/vulkan-systems",
    "third-party/vulkan-wrapper",
    "third-party/vulkan-headers",
    "build/generated"
};

static void compile_shaders(void)
{
    if(pc_build_compile_shaders(paths,"build/shaders"))exit(1);
    const char* shaders[]={"shd/mesh.vert","shd/mesh.frag",
        "shd/optics.frag","shd/cull.comp",
        "third-party/vulkan-systems/optics/optics/shaders/compose.frag",
        "third-party/vulkan-systems/optics/optics/shaders/cache.comp",
        "third-party/vulkan-systems/pmap/shaders/photon_clear.comp",
        "third-party/vulkan-systems/pmap/shaders/photon_emit.comp",
        "third-party/vulkan-systems/pmap/shaders/photon_bin.comp",
        "third-party/vulkan-systems/pmap/shaders/photon_prefix.comp",
        "third-party/vulkan-systems/pmap/shaders/photon_scatter.comp"};
    for(size_t i=0;i<sizeof(shaders)/sizeof(*shaders);++i)
    {
        const char* name=strrchr(shaders[i],'/');
        char output[256],command[640];
        struct stat source,binary;
        snprintf(output,sizeof(output),"build/shaders/%s%s.spv",
                 strstr(shaders[i],"/optics/")?"optics_":
                 strstr(shaders[i],"/pmap/shaders/")?"pmap_":"",
                 name?name+1:shaders[i]);
        if(stat(shaders[i],&source))
        {
            fprintf(stderr,"Missing shader: %s\n",shaders[i]);
            exit(1);
        }
        const char* pmap_header="third-party/vulkan-systems/pmap/shaders/photon_scene.glsl";
        const char* optics_header="third-party/vulkan-systems/optics/optics/shaders/raytrace.glsl";
        const int pmap=strstr(shaders[i],"/pmap/shaders/")!=NULL;
        const int optics=strstr(shaders[i],"/optics/optics/shaders/compose.frag")!=NULL;
        struct stat include;
        if(!stat(output,&binary) && binary.st_mtime>=source.st_mtime &&
           (!pmap || (!stat(pmap_header,&include) &&
                      binary.st_mtime>=include.st_mtime)) &&
           (!optics || (!stat(optics_header,&include) &&
                        binary.st_mtime>=include.st_mtime)))continue;
        snprintf(command,sizeof(command),
                 "glslangValidator -V %s\"%s\" -o \"%s\"",
                 pmap?"-Ithird-party/vulkan-systems/pmap/shaders ":
                 optics?"-Ithird-party/vulkan-systems/optics/optics/shaders ":"",
                 shaders[i],output);
        if(system(command))
        {
            fprintf(stderr,"Shader compilation failed: %s\n",shaders[i]);
            exit(1);
        }
    }
}

static void prepare_editor_assets(void)
{
    struct stat info;
    const char* menu="build/editor-hell2024/Hell2024/Hell2024/res/textures/ui/MenuBG.png";
    if(!stat(menu,&info))return;
    if(stat("build/editor-hell2024/.git",&info) &&
       system("git init -q build/editor-hell2024 && "
              "git -C build/editor-hell2024 remote add origin "
              "https://github.com/livinamuk/Hell2025.git && "
              "git -C build/editor-hell2024 sparse-checkout init --cone"))
    {
        fprintf(stderr,"Could not initialize original editor UI assets\n");
        return;
    }
    if(system("git -C build/editor-hell2024 sparse-checkout set "
              "Hell2024/Hell2024/res/textures/ui && "
              "git -C build/editor-hell2024 fetch -q --depth 1 "
              "--filter=blob:none origin "
              "7362a3a86cc254ecffdf498c9febae10eefa120b && "
              "git -C build/editor-hell2024 checkout -q --detach FETCH_HEAD") ||
       stat(menu,&info))
        fprintf(stderr,"Could not fetch original editor UI textures\n");
}

static C_Target* make_wrapper(C_Build* b)
{
    C_Target* target=c_static_library(b,"VulkanWrapper");
    c_sources(target,"third-party/vulkan-wrapper/vk/*/*.cpp");
    c_generate(target,"build/generated/generated.cpp",
        "third-party/vulkan-headers/registry/vk.xml",
        PYTHON " third-party/vulkan-wrapper/vk/tools/generate.py "
        "third-party/vulkan-headers/registry/vk.xml "
        "build/generated/generated.hpp build/generated/generated.cpp "
        "--headers third-party/vulkan-headers/include");
    pc_build_include_paths(target,paths);
    c_flag(target,"-std=c++17");
#ifdef __linux__
    c_link_system(target,"dl");
#endif
    return target;
}

static C_Target* make_module(C_Build* b,const char* name,const char* folder,
                             C_Target* wrapper)
{
    C_Target* target=pc_build_module(b,name,folder,paths,wrapper);
    char include[128];
    snprintf(include,sizeof(include),"third-party/vulkan-systems/%s",folder);
    c_include(target,include);
    return target;
}

static void game_includes(C_Target* target)
{
    c_include(target,".");
    pc_build_include_paths(target,paths);
    c_include(target,"third-party/vulkan-systems/network");
    c_include(target,"third-party/vulkan-systems/editor");
    c_warnings_strict(target);
    c_flag(target,"-std=c++20");
}

static void link_systems(C_Target* target,const char* const* names,size_t count)
{
    for(size_t i=0;i<count;++i)c_link_system(target,names[i]);
}

void build(C_Build* b)
{
    // Run 'git submodule update --init --recursive' before the first build.
    compile_shaders();
    prepare_editor_assets();

    C_Target* wrapper=make_wrapper(b);
    const pc_build_targets pc=pc_build_register(b,wrapper,paths);
    C_Target* network=make_module(b,"VulkanNetwork","network",NULL);
    C_Target* editor=make_module(b,"VulkanEditor","editor",wrapper);
    C_Target* optics=make_module(b,"VulkanOptics","optics",wrapper);
    C_Target* pmap=make_module(b,"VulkanPMap","pmap",wrapper);
    c_link_target(pmap,pc.gpu);
    c_link_target(pmap,pc.bvh);
    c_link_target(optics,pc.render);
    c_link_target(optics,pc.gpu);

    C_Dependency* crypto=c_git(b,"MbedTLS",
        "https://github.com/Mbed-TLS/mbedtls.git",
        "068ff080b369adfac81509f9b57b2afabaf82dc5");
    c_dep_cmake(crypto);
    c_dep_cmake_option(crypto,"-DENABLE_TESTING=OFF");
    c_dep_cmake_option(crypto,"-DENABLE_PROGRAMS=OFF");
    c_dep_include(crypto,"include");
    const char* crypto_libs[]={"mbedtls","mbedx509","mbedcrypto"};
    for(size_t i=0;i<3;++i)c_dep_link(crypto,crypto_libs[i]);
    c_use(network,crypto);

    C_Dependency* fastgltf=c_git(b,"fastgltf",
        "https://github.com/spnda/fastgltf.git",
        "0b800b4a584704f9e9add67c79c4c8732e552787");
    c_dep_cmake(fastgltf);
    const char* fast_options[]={
        "-DBUILD_SHARED_LIBS=OFF","-DFASTGLTF_ENABLE_INSTALL=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_simdjson=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_simdutf=ON"
    };
    for(size_t i=0;i<4;++i)c_dep_cmake_option(fastgltf,fast_options[i]);
    c_dep_include(fastgltf,"include");
    const char* fast_libs[]={"fastgltf","fastgltf_simdjson","fastgltf_simdutf"};
    for(size_t i=0;i<3;++i)c_dep_link(fastgltf,fast_libs[i]);

    C_Dependency* ozz=c_git(b,"ozz",
        "https://github.com/guillaumeblanc/ozz-animation.git",
        "744eb9d99f606eda849acb0b1204f7a3dc20bca1");
    c_dep_cmake(ozz);
    const char* ozz_options[]={
        "-DBUILD_SHARED_LIBS=OFF","-Dozz_build_tools=OFF",
        "-Dozz_build_samples=OFF","-Dozz_build_howtos=OFF",
        "-Dozz_build_tests=OFF","-Dozz_build_postfix=OFF"
    };
    for(size_t i=0;i<6;++i)c_dep_cmake_option(ozz,ozz_options[i]);
    c_dep_include(ozz,"include");
    const char* ozz_libs[]={
        "ozz_animation_offline","ozz_animation","ozz_geometry","ozz_base"
    };
    for(size_t i=0;i<4;++i)c_dep_link(ozz,ozz_libs[i]);

    C_Dependency* stb=c_git(b,"stb",
        "https://github.com/nothings/stb.git",
        "d14d46f6b44456eb88beac58cba833a4135d4c89");
    c_dep_header_only(stb);
    c_dep_include(stb,".");
    c_use(editor,stb);

    C_Dependency* glfw=c_git(b,"GLFW","https://github.com/glfw/glfw.git","3.4");
    c_dep_cmake(glfw);
    const char* glfw_options[]={
        "-DGLFW_BUILD_EXAMPLES=OFF","-DGLFW_BUILD_TESTS=OFF",
        "-DGLFW_BUILD_DOCS=OFF"
    };
    for(size_t i=0;i<3;++i)c_dep_cmake_option(glfw,glfw_options[i]);
#ifdef __linux__
    c_dep_cmake_option(glfw,"-DGLFW_BUILD_WAYLAND=OFF");
#endif
    c_dep_include(glfw,"include");
    c_dep_link(glfw,"glfw3");

    C_Target* server=c_executable(b,"vulkan-server");
    c_sources(server,"server/server.cpp");
    game_includes(server);
    c_link_target(server,network);
    c_use(server,crypto);
    c_link_target(server,pc.spatial);
    c_link_target(server,pc.jobs);
    c_link_target(server,wrapper);
    c_use(server,fastgltf);
    c_use(server,ozz);
    c_use(server,stb);

    C_Target* client=c_executable(b,"vulkan-client");
    c_sources(client,"client/client.cpp");
    game_includes(client);
    c_include(client,"third-party/vulkan-systems/optics");
    c_include(client,"third-party/vulkan-systems/pmap");
    c_link_target(client,optics);
    c_link_target(client,pmap);
    c_link_target(client,editor);
    pc_build_link(client,pc,wrapper);
    c_link_target(client,network);
    c_use(client,crypto);
    c_use(client,glfw);
    c_use(client,fastgltf);
    c_use(client,ozz);
    c_use(client,stb);

#ifdef __APPLE__
    c_link_system(server,"c++");
    const char* frameworks[]={"Cocoa","IOKit","CoreFoundation","QuartzCore"};
    for(size_t i=0;i<4;++i)c_framework(client,frameworks[i]);
    c_link_system(client,"c++");
#elif defined(__linux__)
    const char* server_libs[]={"pthread","m","stdc++"};
    const char* client_libs[]={
        "X11","Xrandr","Xi","Xcursor","Xinerama","dl","pthread","m","stdc++"
    };
    link_systems(server,server_libs,3);
    link_systems(client,client_libs,9);
#else
    const char* server_libs[]={"ws2_32"};
    const char* client_libs[]={"ws2_32","user32","gdi32","shell32"};
    link_systems(server,server_libs,1);
    link_systems(client,client_libs,4);
#endif
    c_default_target(b,server);
}
