#define VULKAN_GAME_SHARED_ONLY
#include "../server/server.cpp"
#undef VULKAN_GAME_SHARED_ONLY

#include <tuple>
#include <pcloud/pcloud.hpp>
#include <cstdint>
#include <editor/editor.hpp>
#include <array>
#include <vector>
#include <string_view>
#include <string>
#include <vk/vk.hpp>
#include <network/secure.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <network/socket.hpp>
#include <algorithm>
#include <csignal>
#include <cmath>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <render/frame.hpp>
#include <render/sequence.hpp>
#include <gpu/timestamps.hpp>
#include <network/endpoint.hpp>
#include <network/session.hpp>
#include <cstring>
#include <iterator>
#include <random>
#include <cstdlib>
#include <cstddef>
#include <editor/vulkan.hpp>
#include <gpu/graph.hpp>
#include <gpu/upload.hpp>
#include <gpu/culler.hpp>
#include <gpu/resource.hpp>
#include <gpu/descriptors.hpp>
#include <render/renderer.hpp>
#include <render/geometry.hpp>
#include <render/pipeline.hpp>
#include <render/target.hpp>
#include <render/attachments.hpp>
#include <filesystem>
#include <map>
#include <bit>
#include <gpu/texture_cache.hpp>
#include <climits>

#include "clt/animation.hpp"
#include "clt/vulkan_loader.hpp"
#include "clt/window.hpp"
#include "clt/connection.hpp"
#include "clt/prediction.hpp"
#include "clt/camera.hpp"
#include "clt/input.hpp"
#include "clt/texture.hpp"
#include "clt/weapon.hpp"
#include "clt/scene_renderer.hpp"
#include "clt/postprocess.hpp"
#include "clt/renderer.hpp"

static constexpr std::uint64_t editor_light_ids=1ull<<32;
static std::vector<editor::property> light_properties( const ::scene::point_light& light) {
        using kind=editor::property_kind;
        return {
            {"pos_x","Pos X",kind::real,light.position.x,-500,500,0.01,{}},
            {"pos_y","Pos Y",kind::real,light.position.y,-500,500,0.01,{}},
            {"pos_z","Pos Z",kind::real,light.position.z,-500,500,0.01,{}},
            {"color_r","Color X",kind::integer,light.color.x*100,0,100,1,{}},
            {"color_g","Color Y",kind::integer,light.color.y*100,0,100,1,{}},
            {"color_b","Color Z",kind::integer,light.color.z*100,0,100,1,{}},
            {"radius","Radius",kind::real,light.radius,0.05,64,0.1,{}},
            {"strength","Strength",kind::real,light.strength,0,20,0.1,{}}
        };
    }
    static void on_editor_action(const editor::action& action) {
        net::editor_light::resource command{};
        if(action.kind==editor::action_kind::insert) {
            if(action.type!="light")return;
            const auto frame=clt::camera::capture();
            const float yaw=frame.pose.yaw,pitch=frame.pose.pitch;
            const vk::util::math::vec3 forward{
std::sin(yaw)*std::cos(pitch), std::sin(pitch), -std::cos(yaw)*std::cos(pitch)
            };
            command.type=net::editor_light::kind::create;
            command.index=UINT32_MAX;
            command.light.position=frame.eye+forward*2.5f;
            command.light.radius=4.0f;
            command.light.color={1.0f,0.85f,0.7f};
            command.light.strength=1.0f;
        } else if(action.kind==editor::action_kind::edit || action.kind==editor::action_kind::remove) {
if(action.object<=editor_light_ids || action.object>editor_light_ids+64u)return;
const std::size_t index= std::size_t(action.object-editor_light_ids-1u);
            const auto world=clt::connection::snapshot();
            if(!world || index>=world->lights.size())return;
            if(!world->lights[index].enabled)return;
command.type=action.kind==editor::action_kind::remove ? net::editor_light::kind::remove: net::editor_light::kind::edit;
            command.index=std::uint32_t(index);
            command.light=world->lights[index];
            if(command.type==net::editor_light::kind::remove) {
                if(!clt::connection::send_editor_light(command))
                    std::clog<<"clt: could not queue light deletion\n";
                return;
            }
const auto key=std::find(net::editor_light::property_keys.begin(), net::editor_light::property_keys.end(),action.property_id);
            if(key==net::editor_light::property_keys.end())return;
            command.revision=std::uint32_t(key-net::editor_light::property_keys.begin());
*net::editor_light::property_fields(command.light)[command.revision]= command.revision>=3 && command.revision<=5
                ?float(action.value/100.0):float(action.value);
        } else return;
        if(!clt::connection::send_editor_light(command))
            std::clog<<"clt: could not queue editor light command\n";
    }
    static void publish_editor_lights() {
        auto& ui=editor::instance();
        if(!ui.visible())return;
        int width=0,height=0;
        clt::window::framebuffer_size(width,height);
        if(width<=0 || height<=0)return;
        const auto world=clt::connection::snapshot();
        if(!world) {
            ui.set_labels({});
            return;
        }
        using namespace vk::util::math;
const mat4 vp=clt::camera::view_projection( float(width)/float(height));
        std::vector<editor::screen_label> labels;
        labels.reserve(world->lights.size());
        for(std::size_t i=0;i<world->lights.size();++i) {
            const auto& light=world->lights[i];
            if(!light.enabled)continue;
            const vec4 clip=vp*vec4{light.position,1.0f};
if(!std::isfinite(clip.w) || clip.w<=0.0001f || clip.z<0.0f || clip.z>clip.w)
                continue;
            const float nx=clip.x/clip.w,ny=clip.y/clip.w;
if(!std::isfinite(nx)||!std::isfinite(ny) || nx<-1.0f||nx>1.0f||ny<-1.0f||ny>1.0f)
                continue;
            labels.push_back({
editor_light_ids+std::uint64_t(i)+1u, "LIGHT", int(std::lround((nx*0.5f+0.5f)*float(width))), int(std::lround((ny*0.5f+0.5f)*float(height))), true
            });
        }
        ui.set_labels(std::move(labels));
        static std::shared_ptr<const ::scene::state> registered;
        if(registered==world)return;
        registered=world;
        for(std::size_t i=0;i<world->lights.size();++i) {
            const auto& light=world->lights[i];
            if(!light.enabled)continue;
ui.set_properties(editor_light_ids+std::uint64_t(i)+1u, light_properties(light));
        }
    }
    static void on_input(const clt::input::frame& frame) {
        publish_editor_lights();
        if(editor::instance().captures_input()) {
            clt::weapon::controls(false,false,false,0u);
            return;
        }
        static bool space_held=false;
        const bool space=clt::input::key_down(GLFW_KEY_SPACE);
        if(space && !space_held)pc::toggle_probes();
        space_held=space;
        const auto keys=clt::weapon::active_bindings();
        static bool inspect_held=false;
        const bool inspect=clt::input::key_down(keys.inspect_key);
        if(inspect && !inspect_held)clt::weapon::check_magazine();
        inspect_held=inspect;
clt::weapon::controls( clt::input::mouse_down(keys.trigger_button), clt::input::mouse_down(keys.aim_button), clt::input::key_down(keys.reload_key),frame.buttons);
    }
    static void stop(int) {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }

#include "clt/application.hpp"

int main(int argc, char** argv) {
    try {
        game::options::state settings{};
        settings.host = "127.0.0.1";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
        settings.graphics.width = 1280;
        settings.graphics.height = 720;
        settings.graphics.title = "vulkan-game";
        settings.graphics.vertex_shader = "build/shaders/mesh.vert.spv";
        settings.graphics.fragment_shader = "build/shaders/mesh.frag.spv";
        settings.graphics.culling_shader = "build/shaders/cull.comp.spv";
        settings.graphics.lighting_shader = "build/shaders/pcloud_light.comp.spv";
        settings.graphics.tonemap_shader = "build/shaders/pcloud_tone.comp.spv";
        settings.graphics.fullscreen_shader = "build/shaders/pcloud_fullscreen.vert.spv";
        settings.graphics.presentation_shader = "build/shaders/pcloud_present.frag.spv";
        settings.graphics.exposure = 1.0f;
game::options::init(argc, argv, game::options::mode::client_only, std::move(settings));
        if (game::options::get()->help) {
            game::options::print_help();
            return 0;
        }
        clt::weapon::register_weapon(game_weapons::make());
        editor::instance().set_catalog({{"light","Light","Lighting"}});
        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        clt::application::init();
        const int result = clt::application::run();
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    } catch (const std::exception& e) {
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-client: " << e.what() << '\n';
        return 1;
    }
}
