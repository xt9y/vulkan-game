#include "clt/app/application.hpp"
#include "clt/weapon/weapon.hpp"
#include "clt/render/postprocess.hpp"
#include "clt/network/connection.hpp"
#include "clt/platform/window.hpp"
#include "clt/view/camera.hpp"
#include "weapons.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include <network/socket.hpp>
#include <editor/editor.hpp>

#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include <limits>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>


namespace
{
    // The server's scene.light array is append-only today. Reserve a high
    // object-ID range for the editor's projected light labels, rather than
    // reusing mesh instance IDs. The renderer does not need a light mesh.
    constexpr std::uint64_t editor_light_ids=1ull<<32;

    // Define the light's editable fields ONCE, not at every placement site.
    // Every existing and newly created server light uses this schema.
    std::vector<editor::property> light_properties(
        const ::scene::point_light& light)
    {
        using kind=editor::property_kind;
        return {
            {"pos_x","Pos X",kind::real,light.position.x,-500,500,1},
            {"pos_y","Pos Y",kind::real,light.position.y,-500,500,1},
            {"pos_z","Pos Z",kind::real,light.position.z,-500,500,1},
            // Colors are displayed as percentages so arrow increments are
            // exactly 1 or 10 without jumping across normalized [0,1].
            {"color_r","Color X",kind::integer,light.color.x*100,0,100,1},
            {"color_g","Color Y",kind::integer,light.color.y*100,0,100,1},
            {"color_b","Color Z",kind::integer,light.color.z*100,0,100,1},
            {"radius","Radius",kind::real,light.radius,0.05,64,1},
            {"strength","Strength",kind::real,light.strength,0,20,1}
        };
    }

    void on_editor_action(const editor::action& action)
    {
        net::editor_light::resource command{};
        if(action.kind==editor::action_kind::insert)
        {
            if(action.type!="light")return;
            const auto frame=clt::camera::capture();
            const float yaw=frame.pose.yaw,pitch=frame.pose.pitch;
            const vk::util::math::vec3 forward{
                std::sin(yaw)*std::cos(pitch),
                std::sin(pitch),
                -std::cos(yaw)*std::cos(pitch)
            };
            command.type=net::editor_light::kind::create;
            command.index=UINT32_MAX;
            command.light.position=frame.eye+forward*2.5f;
            command.light.radius=4.0f;
            command.light.color={1.0f,0.85f,0.7f};
            command.light.strength=1.0f;
        }
        else if(action.kind==editor::action_kind::edit ||
                action.kind==editor::action_kind::remove)
        {
            if(action.object<=editor_light_ids ||
               action.object>editor_light_ids+64u)return;
            const std::size_t index=
                std::size_t(action.object-editor_light_ids-1u);
            const auto world=clt::connection::snapshot();
            if(!world || index>=world->lights.size())return;
            if(!world->lights[index].enabled)return;
            command.type=action.kind==editor::action_kind::remove ?
                net::editor_light::kind::remove:
                net::editor_light::kind::edit;
            command.index=std::uint32_t(index);
            command.light=world->lights[index];
            if(command.type==net::editor_light::kind::remove)
            {
                if(!clt::connection::send_editor_light(command))
                    std::clog<<"clt: could not queue light deletion\n";
                return;
            }
            // revision in client edit commands identifies the property:
            // the server changes that field only, then replicates the
            // entire authoritative point_light back to all clients.
            const auto& key=action.property_id;
            if(key=="pos_x")
                command.revision=0,command.light.position.x=float(action.value);
            else if(key=="pos_y")
                command.revision=1,command.light.position.y=float(action.value);
            else if(key=="pos_z")
                command.revision=2,command.light.position.z=float(action.value);
            else if(key=="color_r")
                command.revision=3,command.light.color.x=float(action.value/100.0);
            else if(key=="color_g")
                command.revision=4,command.light.color.y=float(action.value/100.0);
            else if(key=="color_b")
                command.revision=5,command.light.color.z=float(action.value/100.0);
            else if(key=="radius")
                command.revision=6,command.light.radius=float(action.value);
            else if(key=="strength")
                command.revision=7,command.light.strength=float(action.value);
            else return;
        }
        else return;

        if(!clt::connection::send_editor_light(command))
            std::clog<<"clt: could not queue editor light command\n";
    }


    void publish_editor_lights()
    {
        auto& ui=editor::instance();
        if(!ui.visible())return;

        int width=0,height=0;
        clt::window::framebuffer_size(width,height);
        if(width<=0 || height<=0)return;

        const auto world=clt::connection::snapshot();
        if(!world)
        {
            ui.set_labels({});
            return;
        }

        using namespace vk::util::math;
        const mat4 vp=clt::camera::view_projection(
            float(width)/float(height));
        std::vector<editor::screen_label> labels;
        labels.reserve(world->lights.size());
        for(std::size_t i=0;i<world->lights.size();++i)
        {
            const auto& light=world->lights[i];
            if(!light.enabled)continue;
            const vec4 clip=vp*vec4{light.position,1.0f};
            // Vulkan NDC Z is [0,1]. Projection already flips Y to
            // framebuffer-down coordinates. Never draw behind the camera.
            if(!std::isfinite(clip.w) || clip.w<=0.0001f ||
               clip.z<0.0f || clip.z>clip.w)
                continue;
            const float nx=clip.x/clip.w,ny=clip.y/clip.w;
            if(!std::isfinite(nx)||!std::isfinite(ny) ||
               nx<-1.0f||nx>1.0f||ny<-1.0f||ny>1.0f)
                continue;
            labels.push_back({
                editor_light_ids+std::uint64_t(i)+1u,
                "LIGHT",
                int(std::lround((nx*0.5f+0.5f)*float(width))),
                int(std::lround((ny*0.5f+0.5f)*float(height))),
                true
            });
        }
        ui.set_labels(std::move(labels));

        // Only refresh properties when a new authoritative snapshot arrives.
        // Otherwise arrow-key UI edits would be reset every input frame.
        static std::shared_ptr<const ::scene::state> registered;
        if(registered==world)return;
        registered=world;
        for(std::size_t i=0;i<world->lights.size();++i)
        {
            const auto& light=world->lights[i];
            if(!light.enabled)continue;
            ui.set_properties(editor_light_ids+std::uint64_t(i)+1u,
                              light_properties(light));
        }
    }

    void load_weapons()
    {
        for(auto&& gun:game_weapons::all())
            clt::weapon::register_weapon(std::move(gun));
    }

    void on_input(const clt::input::frame& frame)
    {
        // Publish the real server-streamed light positions to the existing
        // Hell2024 Icon_Light overlay. UI input and shader lights are separate.
        publish_editor_lights();
        // Clicking editor world labels must never fire the active weapon.
        if(editor::instance().captures_input())
        {
            clt::weapon::controls(false,false,false,0u);
            return;
        }
        static bool space_held=false;
        const bool space=clt::input::key_down(GLFW_KEY_SPACE);
        if(space && !space_held)clt::postprocess::toggle_probes();
        space_held=space;

        // Each gun chooses its own equip key, trigger, ADS and reload input.
        static std::array<bool,GLFW_KEY_LAST+1> equip_held{};
        for(const auto& item:clt::weapon::equip_bindings())
        {
            if(item.key<0 || item.key>GLFW_KEY_LAST)continue;
            const bool down=clt::input::key_down(item.key);
            if(down && !equip_held[item.key])
                clt::weapon::equip(item.id);
            equip_held[item.key]=down;
        }
        const auto keys=clt::weapon::active_bindings();
        static bool inspect_held=false;
        const bool inspect=clt::input::key_down(keys.inspect_key);
        if(inspect && !inspect_held)clt::weapon::check_magazine();
        inspect_held=inspect;
        clt::weapon::controls(
            clt::input::mouse_down(keys.trigger_button),
            clt::input::mouse_down(keys.aim_button),
            clt::input::key_down(keys.reload_key),frame.buttons);
    }

    void stop(int)
    {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }
}

int main(int argc, char** argv)
{
    try
    {
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
        settings.graphics.surface = game::options::surface_format::bgra8_srgb;
        settings.graphics.depth = game::options::depth_precision::d32;
        settings.graphics.present = game::options::presentation::fifo;
        settings.graphics.gbuffer = game::options::gbuffer_precision::rgba8;
        settings.graphics.culling_shader = "build/shaders/cull.comp.spv";
        settings.graphics.lighting_shader = "build/shaders/light.comp.spv";
        settings.graphics.tonemap_shader = "build/shaders/tone.comp.spv";
        settings.graphics.fullscreen_shader = "build/shaders/fullscreen.vert.spv";
        settings.graphics.presentation_shader = "build/shaders/present.frag.spv";
        settings.graphics.exposure = 1.0f;

        game::options::init(argc, argv, game::options::mode::client_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
            game::options::print_help();
            return 0;
        }

        load_weapons();
        // Host-defined editor catalog; LIGHT properties are auto-generated
        // for every light from the single schema above.
        editor::instance().set_catalog({{"light","Light","Lighting"}});
        clt::application::set_callbacks({on_input,on_editor_action});
        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        clt::application::init();
        const int result = clt::application::run();
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-client: " << e.what() << '\n';
        return 1;
    }
}
