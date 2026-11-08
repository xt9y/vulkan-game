#include <cbuild.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#define PYTHON "python"
#else
#define PYTHON "python3"
#endif

static void includes(C_Target* t)
{
    c_include(t,"third-party/vulkan-wrapper");
    c_include(t,"third-party/vulkan-wrapper/vk");
    c_include(t,"third-party/vulkan-headers/include");
    c_include(t,"build/generated");
    c_include(t,"third-party/vulkan-systems/network");
    c_include(t,"third-party/vulkan-systems/pcloud");
}
void build(C_Build* b)
{
    const char* required[]={
        "third-party/vulkan-wrapper/vk/tools/generate.py",
        "third-party/vulkan-systems/network/network/secure.cpp",
        "third-party/vulkan-systems/pcloud/pcloud/pcloud.hpp",
        "third-party/vulkan-headers/registry/vk.xml"
    };
    for(unsigned i=0;i<sizeof(required)/sizeof(*required);++i)
    {
        struct stat st;
        if(stat(required[i],&st)!=0)
        {
            fprintf(stderr,"Missing submodule source %s; run git submodule update --init --recursive\n",required[i]);
            exit(1);
        }
    }
    C_Target* wrapper=c_static_library(b,"VulkanWrapper");
    c_sources(wrapper,"third-party/vulkan-wrapper/vk/*/*.cpp");
    c_generate(wrapper,"build/generated/generated.cpp",
        "third-party/vulkan-headers/registry/vk.xml",
        PYTHON " third-party/vulkan-wrapper/vk/tools/generate.py "
        "third-party/vulkan-headers/registry/vk.xml "
        "build/generated/generated.hpp build/generated/generated.cpp "
        "--headers third-party/vulkan-headers/include");
    includes(wrapper);
    c_flag(wrapper,"-std=c++17");
#ifdef __linux__
    c_link_system(wrapper,"dl");
#endif
    C_Target* network=c_static_library(b,"VulkanNetwork");
    c_sources(network,"third-party/vulkan-systems/network/network/*.cpp");
    includes(network);
    c_flag(network,"-std=c++17");
    C_Dependency* crypto=c_git(b,"MbedTLS",
        "https://github.com/Mbed-TLS/mbedtls.git",
        "068ff080b369adfac81509f9b57b2afabaf82dc5");
    c_dep_cmake(crypto);
    c_dep_cmake_option(crypto,"-DENABLE_TESTING=OFF");
    c_dep_cmake_option(crypto,"-DENABLE_PROGRAMS=OFF");
    c_dep_include(crypto,"include");
    c_dep_link(crypto,"mbedtls");
    c_dep_link(crypto,"mbedx509");
    c_dep_link(crypto,"mbedcrypto");
    c_use(network,crypto);
    C_Target* server=c_executable(b,"vulkan-server");
    c_sources(server,"server.cpp");
    includes(server);
    c_flag(server,"-std=c++20");
    c_link_target(server,network);
    c_use(server,crypto);
    C_Target* client=c_executable(b,"vulkan-client");
    c_sources(client,"client.cpp");
    includes(client);
    c_flag(client,"-std=c++20");
    c_link_target(client,network);
    c_use(client,crypto);
    c_link_target(client,wrapper);
#ifdef __APPLE__
    c_link_system(server,"c++");
    c_link_system(client,"c++");
#elif defined(__linux__)
    c_link_system(server,"pthread");
    c_link_system(server,"stdc++");
    c_link_system(client,"pthread");
    c_link_system(client,"stdc++");
#else
    c_link_system(server,"ws2_32");
    c_link_system(client,"ws2_32");
#endif
    c_default_target(b,server);
}
