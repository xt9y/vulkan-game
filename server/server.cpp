#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <span>
#include <string>
#include <limits>
#include <spatial/index.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <filesystem>
#include <vk/vk.hpp>
#include <fstream>
#include <iterator>
#include <regex>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include <climits>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>
#include <fastgltf/core.hpp>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <cstring>
#include <numeric>
#include <iostream>
#include <atomic>
#include <string_view>
#include <network/secure.hpp>
#include <cstdlib>
#include <network/endpoint.hpp>
#include <network/session.hpp>
#include <functional>
#include <memory>
#include <network/socket.hpp>
#include <csignal>
#include <exception>
#include <jobs/pool.hpp>
#include <chrono>
#include <deque>
#include <thread>
#include "util/math/vector.hpp"
#include "util/math/quaternion.hpp"

#include "net/movement/detail.hpp"
#include "net/message.hpp"
#include "scene.hpp"
#include "weapon.hpp"
#include "game_weapons.hpp"
#include "net/scene_stream.hpp"
#include "srv/assets.hpp"
#include "net/animation.hpp"
#include "net/limits.hpp"
#include "player.hpp"
#include "net/movement.hpp"
#include "net/weapon.hpp"
#include "net/editor_light.hpp"
#include "game/options.hpp"
#include "game/application.hpp"
#include "game/secure_config.hpp"

#ifndef VULKAN_GAME_SHARED_ONLY
#include "srv/session.hpp"
#include "srv/application.hpp"

static ::scene::mesh box_mesh(std::uint32_t id,vk::util::math::vec3 h, bool checker=false) {
        using vk::util::math::vec3;
        ::scene::mesh mesh{};
        mesh.id=id;
        mesh.surface.base_color={1,1,1,1};
        mesh.surface.roughness=.85f;
        mesh.surface.procedural_pattern=checker?1u:0u;
        const vec3 p[8]={
            {-h.x,-h.y,-h.z},{ h.x,-h.y,-h.z},
            { h.x, h.y,-h.z},{-h.x, h.y,-h.z},
            {-h.x,-h.y, h.z},{ h.x,-h.y, h.z},
            { h.x, h.y, h.z},{-h.x, h.y, h.z}
        };
        const int faces[6][4]={
            {4,5,6,7},{1,0,3,2},{0,4,7,3},
            {5,1,2,6},{7,6,2,3},{0,1,5,4}
        };
        const vec3 normals[6]={
            {0,0,1},{0,0,-1},{-1,0,0},
            {1,0,0},{0,1,0},{0,-1,0}
        };
        for(std::uint32_t f=0;f<6;++f) {
            const auto start=std::uint32_t(mesh.vertices.size());
            for(int j=0;j<4;++j)
                mesh.vertices.push_back({
                    p[faces[f][j]],normals[f],float(j==1||j==2),float(j>=2)
                });
            mesh.indices.insert(mesh.indices.end(),{
                start,start+1,start+2,start,start+2,start+3
            });
        }
        return mesh;
    }
    static ::scene::state make_level() {
        using vk::util::math::vec3;
        ::scene::state world{};
        world.meshes.push_back(box_mesh(1,{10,.1f,10},true));
        world.meshes.push_back(box_mesh(2,{.5f,.5f,.5f}));
const auto place=[&](std::uint32_t id,std::uint32_t mesh, vec3 pos,vec3 half) {
            ::scene::instance instance{};
            instance.id=id;
            instance.mesh_id=mesh;
            instance.pose.position=pos;
            world.instances.push_back(instance);
            world.collision_boxes.push_back({pos-half,pos+half});
        };
        place(1,1,{0,-.1f,0},{10,.1f,10});
        place(2,2,{-2,.5f,0},{.5f,.5f,.5f});
        place(3,2,{0,.5f,0},{.5f,.5f,.5f});
        place(4,2,{2,.5f,0},{.5f,.5f,.5f});
        ::scene::transform mermaid{};
        mermaid.position={0,1,.5f/3.f};
        mermaid.scale={1.5f,1.5f,1.5f};
        srv::application::load_model("assets/mermaid/mermaid.glb",mermaid,0);
        world.lights.push_back({{0,3,1},6,{1,.5f,.2f},4});
        return world;
    }
    static void stop(int) {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }

int main(int argc, char** argv) {
    try {
        game::options::state settings{};
        settings.bind_host = "0.0.0.0";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
game::options::init(argc, argv, game::options::mode::server_only, std::move(settings));
        if (game::options::get()->help) {
            game::options::print_help();
            return 0;
        }
        player::configure({
            {0, 0, 4}, // spawn
            4.0f,      // walking speed
            14.0f,     // sprint speed
            20.0f,     // gravity
            0.3f,      // player radius (collision width)
            1.8f       // player height (collision and camera)
        });
        srv::application::add_weapon(game_weapons::make().rules);
        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        srv::application::set_scene(make_level());
        srv::application::init();
        const int result = srv::application::run();
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    } catch (const std::exception& e) {
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-server: " << e.what() << '\n';
        return 1;
    }
}
#endif
