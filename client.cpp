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
            ui.set_properties(editor_light_ids+std::uint64_t(i)+1u,{
                {"pos_x","Pos X",editor::property_kind::real,
                    light.position.x,-10000.0,10000.0,0.1},
                {"pos_y","Pos Y",editor::property_kind::real,
                    light.position.y,-10000.0,10000.0,0.1},
                {"pos_z","Pos Z",editor::property_kind::real,
                    light.position.z,-10000.0,10000.0,0.1},
                {"color_r","Color X",editor::property_kind::real,
                    light.color.x,0.0,1.0,0.1},
                {"color_g","Color Y",editor::property_kind::real,
                    light.color.y,0.0,1.0,0.1},
                {"color_b","Color Z",editor::property_kind::real,
                    light.color.z,0.0,1.0,0.1},
                {"radius","Radius",editor::property_kind::real,
                    light.radius,0.0,1000.0,0.1},
                {"strength","Strength",editor::property_kind::real,
                    light.strength,0.0,1000.0,0.1}
            });
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
        clt::application::set_callbacks({on_input});
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
