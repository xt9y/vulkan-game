#include "clt/animation.hpp"
#include "clt/camera.hpp"
#include "clt/connection.hpp"
#include "clt/input.hpp"
#include "clt/postprocess.hpp"
#include "clt/prediction.hpp"
#include "clt/renderer.hpp"
#include "clt/scene_renderer.hpp"
#include "clt/texture.hpp"
#include "clt/vulkan_loader.hpp"
#include "clt/weapon.hpp"
#include "clt/window.hpp"
#include "../server/game/application.hpp"
#include "../server/game/options.hpp"
#include "../server/game_weapons.hpp"
#include <csignal>

static constexpr std::uint64_t editor_light_ids = 1ull << 32;
static constexpr std::uint64_t editor_cube_ids = 2ull << 32;
static std::vector<editor::property> light_properties(const ::scene::point_light &light)
{
    using kind = editor::property_kind;

    return {
        {"pos_x", "Pos X", kind::real, light.position.x, -500, 500, 0.01, {}},
        {"pos_y", "Pos Y", kind::real, light.position.y, -500, 500, 0.01, {}},
        {"pos_z", "Pos Z", kind::real, light.position.z, -500, 500, 0.01, {}},
        {"color_r", "Color X", kind::integer, light.color.x * 100, 0, 100, 1, {}},
        {"color_g", "Color Y", kind::integer, light.color.y * 100, 0, 100, 1, {}},
        {"color_b", "Color Z", kind::integer, light.color.z * 100, 0, 100, 1, {}},
        {"radius", "Radius", kind::real, light.radius, 0.05, 64, 0.1, {}},
        {"strength", "Strength", kind::real, light.strength, 0, 20, 0.1, {}}
    };
}

static std::vector<editor::property> geometry_properties(const ::scene::instance &cube)
{
    using kind = editor::property_kind;
    const auto angles = net::editor_cube::angles(cube);
    std::vector<editor::property> properties{
        {"pos_x", "Pos X", kind::real, cube.pose.position.x, -500, 500, 0.01, {}},
        {"pos_y", "Pos Y", kind::real, cube.pose.position.y, -500, 500, 0.01, {}},
        {"pos_z", "Pos Z", kind::real, cube.pose.position.z, -500, 500, 0.01, {}},
        {"rot_x", "Rotation X", kind::real, angles.x, -180, 180, 1, {}},
        {"rot_y", "Rotation Y", kind::real, angles.y, -180, 180, 1, {}},
        {"rot_z", "Rotation Z", kind::real, angles.z, -180, 180, 1, {}}
    };
    if(cube.editor_group) {
        properties.push_back({"scale", "Scale", kind::real,
                              cube.pose.scale.x, .01, 100, .01, {}});
        return properties;
    }
    const float width=cube.mesh_id==::scene::ground_mesh_id?20.f:1.f;
    const float height=cube.mesh_id==::scene::ground_mesh_id?.2f:1.f;
    const float depth=width;
    properties.push_back({"width","Width",kind::real,cube.pose.scale.x*width,
                          .01,100,.01,{}});
    properties.push_back({"height","Height",kind::real,cube.pose.scale.y*height,
                          .01,100,.01,{}});
    properties.push_back({"depth","Depth",kind::real,cube.pose.scale.z*depth,
                          .01,100,.01,{}});
    properties.push_back({"texture","Texture",kind::choice,
        double(cube.pattern<=3?cube.pattern:
               (cube.mesh_id==::scene::ground_mesh_id?1u:0u)),0,3,1,
        {"Solid","Generated checker","Glass","Reflective"}});
    properties.push_back({"color_r","Color R",kind::integer,cube.tint.x*100,
                          0,100,1,{}});
    properties.push_back({"color_g","Color G",kind::integer,cube.tint.y*100,
                          0,100,1,{}});
    properties.push_back({"color_b","Color B",kind::integer,cube.tint.z*100,
                          0,100,1,{}});
    if(cube.pattern==2u) {
        properties.push_back({"optical_strength","Pass through",
            kind::real,cube.optical_strength,0,1,.01,{}});
        properties.push_back({"optical_mattness","Mattness",
            kind::real,cube.optical_mattness,0,1,.01,{}});
    }
    return properties;
}

static void on_geometry_action(const editor::action &action)
{
    using namespace net::editor_cube;
    resource command{};
    command.cube.mesh_id = action.type=="sphere" ?
        ::scene::unit_sphere_mesh_id :
        action.type=="mermaid" ? ::scene::mermaid_insert_mesh_id :
        ::scene::unit_cube_mesh_id;
    if (action.kind == editor::action_kind::insert) {
        const auto frame = clt::camera::capture();
        const auto forward=::weapon::direction(frame.pose.yaw,frame.pose.pitch);
        command.type = kind::create;
        command.index = UINT32_MAX;
        command.cube.pose.position = frame.eye + forward * 2.5f;
        if(action.type=="mermaid")
            command.cube.pose.scale={1.5f,1.5f,1.5f};
        command.cube.pattern = action.type=="mermaid"?UINT32_MAX:0u;
    }
    else if (action.kind == editor::action_kind::edit || action.kind == editor::action_kind::remove) {
        const auto world = clt::connection::snapshot();
        if (!world) return;
        command.index = std::uint32_t(action.object - editor_cube_ids);
        const auto found = std::find_if(world->instances.begin(), world->instances.end(), [&](const auto &i) {
            return i.id==command.index &&
                (i.mesh_id==::scene::ground_mesh_id ||
                 i.mesh_id==::scene::unit_cube_mesh_id ||
                 i.mesh_id==::scene::unit_sphere_mesh_id ||
                 i.editor_group!=0);
        });
        if (found == world->instances.end()) return;
        command.cube = *found;
        command.type = action.kind == editor::action_kind::remove ? kind::remove : kind::edit;
        if (command.type == kind::edit) {
            const auto key = std::find(property_keys.begin(), property_keys.end(), action.property_id);
            if (key == property_keys.end()) return;
            command.revision = std::uint32_t(key - property_keys.begin());
            command.value = float(action.value);
            if (!set_property(command.cube, command.revision, command.value)) return;
        }
    }
    else return;
    if (!clt::connection::send_editor_cube(command))
        std::clog << "clt: could not queue editor cube command\n";
}

static void on_editor_action(const editor::action &action)
{
    if ((action.kind==editor::action_kind::insert &&
         (action.type=="cube" || action.type=="sphere" ||
          action.type=="mermaid")) ||
        (action.object > editor_cube_ids &&
         action.object < editor_cube_ids + UINT32_MAX)) {
        on_geometry_action(action);
        return;
    }
    net::editor_light::resource command{};

    if (action.kind == editor::action_kind::insert) {
        if (action.type != "light") return;

        const auto frame = clt::camera::capture();
        const auto forward=::weapon::direction(frame.pose.yaw,frame.pose.pitch);

        command.type = net::editor_light::kind::create;
        command.index = UINT32_MAX;
        command.light.position = frame.eye + forward * 2.5f;
        command.light.radius = 4.0f;
        command.light.color = {1.0f, 0.85f, 0.7f};
        command.light.strength = 1.0f;
    }
    else if (action.kind == editor::action_kind::edit || action.kind == editor::action_kind::remove) {
        if (action.object <= editor_light_ids || action.object > editor_light_ids + 64u) return;
        const std::size_t index = std::size_t(action.object - editor_light_ids - 1u);

        const auto world = clt::connection::snapshot();
        if (!world || index >= world->lights.size()) return;
        if (!world->lights[index].enabled) return;
        command.type = action.kind == editor::action_kind::remove ? net::editor_light::kind::remove : net::editor_light::kind::edit;
        command.index = std::uint32_t(index);
        command.light = world->lights[index];

        if (command.type == net::editor_light::kind::remove) {
            if (!clt::connection::send_editor_light(command))
                std::clog << "clt: could not queue light deletion\n";

            return;
        }

        const auto key = std::find(net::editor_light::property_keys.begin(), net::editor_light::property_keys.end(), action.property_id);

        if (key == net::editor_light::property_keys.end()) return;
        command.revision = std::uint32_t(key - net::editor_light::property_keys.begin());
        *net::editor_light::property_fields(command.light)[command.revision] = command.revision >= 3 && command.revision <= 5
                                                                                   ? float(action.value / 100.0)
                                                                                   : float(action.value);
    }
    else
        return;

    if (!clt::connection::send_editor_light(command))
        std::clog << "clt: could not queue editor light command\n";
}

static void publish_editor_objects()
{
    auto &ui = editor::instance();

    if (!ui.visible()) return;

    int width = 0, height = 0;
    clt::window::framebuffer_size(width, height);

    if (width <= 0 || height <= 0) return;

    const auto world = clt::connection::snapshot();
    static std::shared_ptr<const ::scene::state> registered;
    static std::vector<std::size_t> editable;
    if (!world) {
        registered.reset();
        editable.clear();
        ui.set_labels({});
        return;
    }
    const bool changed=registered!=world;
    if(changed) {
        editable.clear();
        for(std::size_t i=0;i<world->instances.size();++i) {
            const auto& cube=world->instances[i];
            if(cube.editor_group && cube.id!=cube.editor_group)continue;
            if(cube.mesh_id==::scene::ground_mesh_id ||
               cube.mesh_id==::scene::unit_cube_mesh_id ||
               cube.mesh_id==::scene::unit_sphere_mesh_id ||
               cube.editor_group)
                editable.push_back(i);
        }
        registered=world;
    }

    using namespace vk::util::math;

    const mat4 vp = clt::camera::view_projection(float(width) / float(height));

    std::vector<editor::screen_label> labels;
    labels.reserve(world->lights.size()+editable.size());
    const auto add_label=[&](std::uint64_t id,const char* text,vec3 position) {
        const vec4 clip=vp*vec4{position,1.f};
        if(!std::isfinite(clip.w) || clip.w<=.0001f ||
           clip.z<0.f || clip.z>clip.w)return;
        const float nx=clip.x/clip.w,ny=clip.y/clip.w;
        if(!std::isfinite(nx) || !std::isfinite(ny) ||
           std::abs(nx)>1.f || std::abs(ny)>1.f)return;
        labels.push_back({id,text,
            int(std::lround((nx*.5f+.5f)*width)),
            int(std::lround((ny*.5f+.5f)*height)),true});
    };
    for(std::size_t i=0;i<world->lights.size();++i)
        if(world->lights[i].enabled)
            add_label(editor_light_ids+std::uint64_t(i)+1u,
                      "LIGHT",world->lights[i].position);
    for(const auto index:editable) {
        const auto& cube=world->instances[index];
        add_label(editor_cube_ids+cube.id,
            cube.editor_group?"MERMAID":
            cube.mesh_id==::scene::unit_sphere_mesh_id?"SPHERE":"CUBE",
            cube.pose.position);
    }
    ui.set_labels(std::move(labels));

    if (!changed) return;

    for (std::size_t i = 0; i < world->lights.size(); ++i)
    {
        const auto &light = world->lights[i];

        if (!light.enabled) continue;
        ui.set_properties(editor_light_ids + std::uint64_t(i) + 1u, light_properties(light));
    }
    for(const auto index:editable) {
        const auto& cube=world->instances[index];
        ui.set_properties(editor_cube_ids+cube.id,geometry_properties(cube));
    }

}

static void on_input(const clt::input::frame &frame)
{
    publish_editor_objects();

    if (editor::instance().captures_input()) {
        clt::weapon::controls(false, false, false, 0u);

        return;
    }

    static bool space_held = false;
    const bool space = clt::input::key_down(GLFW_KEY_SPACE);
    if (space && !space_held) pc::toggle_probes();
    space_held = space;

    const auto keys = clt::weapon::active_bindings();
    static bool inspect_held = false;
    const bool inspect = clt::input::key_down(keys.inspect_key);
    if (inspect && !inspect_held) clt::weapon::check_magazine();
    inspect_held = inspect;
    clt::weapon::controls(clt::input::mouse_down(keys.trigger_button), clt::input::mouse_down(keys.aim_button), clt::input::key_down(keys.reload_key), frame.buttons);
}

static void stop(int)
{
    game::application::get()->stop.store(true, std::memory_order_relaxed);
}

#include "clt/application.hpp"

int main(int argc, char **argv)
{
    try
    {
        game::options::state settings{};
        settings.host = "127.0.0.1";
        settings.port = 27015;
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
        editor::instance().set_catalog({
            {"light","Light","Lighting"},
            {"cube","Cube","Geometry"},
            {"sphere","Sphere","Geometry"},
            {"mermaid","Mermaid","Geometry"}
        });
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

    catch (const std::exception &e)
    {
        clt::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-client: " << e.what() << '\n';

        return 1;
    }
}
