#define VULKAN_GAME_SHARED_ONLY
#include "../server/server.cpp"
#undef VULKAN_GAME_SHARED_ONLY

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <editor/editor.hpp>
#include <editor/vulkan.hpp>
#include <exception>
#include <filesystem>
#include <gpu/culler.hpp>
#include <gpu/descriptors.hpp>
#include <gpu/graph.hpp>
#include <gpu/resource.hpp>
#include <gpu/texture_cache.hpp>
#include <gpu/timestamps.hpp>
#include <gpu/upload.hpp>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <network/endpoint.hpp>
#include <network/secure.hpp>
#include <network/session.hpp>
#include <network/socket.hpp>
#include <optional>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <pcloud/pcloud.hpp>
#include <random>
#include <render/attachments.hpp>
#include <render/frame.hpp>
#include <render/geometry.hpp>
#include <render/pipeline.hpp>
#include <render/renderer.hpp>
#include <render/sequence.hpp>
#include <render/target.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vk/vk.hpp>

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

static std::vector<editor::property> cube_properties(const ::scene::instance &cube)
{
    using kind = editor::property_kind;
    const auto angles = net::editor_cube::angles(cube);
    std::vector<editor::property> properties{
        {"pos_x", "Pos X", kind::real, cube.pose.position.x, -500, 500, 0.01, {}},
        {"pos_y", "Pos Y", kind::real, cube.pose.position.y, -500, 500, 0.01, {}},
        {"pos_z", "Pos Z", kind::real, cube.pose.position.z, -500, 500, 0.01, {}},
        {"rot_x", "Rotation X", kind::real, angles.x, -180, 180, 1, {}},
        {"rot_y", "Rotation Y", kind::real, angles.y, -180, 180, 1, {}},
        {"rot_z", "Rotation Z", kind::real, angles.z, -180, 180, 1, {}},
        {"width", "Width", kind::real, cube.pose.scale.x, 0.01, 100, 0.01, {}},
        {"height", "Height", kind::real, cube.pose.scale.y, 0.01, 100, 0.01, {}},
        {"depth", "Depth", kind::real, cube.pose.scale.z, 0.01, 100, 0.01, {}},
        {"texture", "Texture", kind::choice, double(cube.pattern <= 3 ? cube.pattern : 0), 0, 3, 1, {"Solid", "Generated checker", "Glass", "Reflective"}},
        {"color_r", "Color R", kind::integer, cube.tint.x * 100, 0, 100, 1, {}},
        {"color_g", "Color G", kind::integer, cube.tint.y * 100, 0, 100, 1, {}},
        {"color_b", "Color B", kind::integer, cube.tint.z * 100, 0, 100, 1, {}}
    };
    if(cube.pattern==2u || cube.pattern==3u) {
        properties.push_back({"optical_strength",
            cube.pattern==2u ? "Pass through" : "Reflectivity",
            kind::real,cube.optical_strength,0,1,.01,{}});
        properties.push_back({"optical_mattness","Mattness",
            kind::real,cube.optical_mattness,0,1,.01,{}});
    }
    return properties;
}

static void on_cube_action(const editor::action &action)
{
    using namespace net::editor_cube;
    resource command{};
    command.cube.mesh_id = ::scene::unit_cube_mesh_id;
    if (action.kind == editor::action_kind::insert) {
        const auto frame = clt::camera::capture();
        const auto yaw = frame.pose.yaw, pitch = frame.pose.pitch;
        const vk::util::math::vec3 forward{
            std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)
        };
        command.type = kind::create;
        command.index = UINT32_MAX;
        command.cube.pose.position = frame.eye + forward * 2.5f;
        command.cube.pattern = 0;
    }
    else if (action.kind == editor::action_kind::edit || action.kind == editor::action_kind::remove) {
        const auto world = clt::connection::snapshot();
        if (!world) return;
        command.index = std::uint32_t(action.object - editor_cube_ids);
        const auto found = std::find_if(world->instances.begin(), world->instances.end(), [&](const auto &i) {
            return i.id == command.index && i.mesh_id == ::scene::unit_cube_mesh_id;
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
    if ((action.kind == editor::action_kind::insert && action.type == "cube") ||
        (action.object > editor_cube_ids && action.object < editor_cube_ids + UINT32_MAX)) {
        on_cube_action(action);
        return;
    }
    net::editor_light::resource command{};

    if (action.kind == editor::action_kind::insert) {
        if (action.type != "light") return;

        const auto frame = clt::camera::capture();
        const float yaw = frame.pose.yaw, pitch = frame.pose.pitch;

        const vk::util::math::vec3 forward{
            std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)
        };

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

    if (!world) {
        ui.set_labels({});

        return;
    }

    using namespace vk::util::math;

    const mat4 vp = clt::camera::view_projection(float(width) / float(height));

    std::vector<editor::screen_label> labels;
    labels.reserve(world->lights.size());

    for (std::size_t i = 0; i < world->lights.size(); ++i)
    {
        const auto &light = world->lights[i];

        if (!light.enabled) continue;

        const vec4 clip = vp * vec4{light.position, 1.0f};

        if (!std::isfinite(clip.w) || clip.w <= 0.0001f || clip.z < 0.0f || clip.z > clip.w)
            continue;

        const float nx = clip.x / clip.w, ny = clip.y / clip.w;

        if (!std::isfinite(nx) || !std::isfinite(ny) || nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f)
            continue;
        labels.push_back({editor_light_ids + std::uint64_t(i) + 1u, "LIGHT", int(std::lround((nx * 0.5f + 0.5f) * float(width))), int(std::lround((ny * 0.5f + 0.5f) * float(height))), true});
    }

    for (const auto &cube : world->instances) {
        if (cube.mesh_id != ::scene::unit_cube_mesh_id) continue;
        const vec4 clip = vp * vec4{cube.pose.position, 1.f};
        if (!std::isfinite(clip.w) || clip.w <= .0001f || clip.z < 0.f || clip.z > clip.w) continue;
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        if (!std::isfinite(nx) || !std::isfinite(ny) || std::abs(nx) > 1.f || std::abs(ny) > 1.f) continue;
        labels.push_back({editor_cube_ids + cube.id, "CUBE", int(std::lround((nx * .5f + .5f) * width)),
                          int(std::lround((ny * .5f + .5f) * height)), true});
    }
    ui.set_labels(std::move(labels));
    static std::shared_ptr<const ::scene::state> registered;

    if (registered == world) return;
    registered = world;

    for (std::size_t i = 0; i < world->lights.size(); ++i)
    {
        const auto &light = world->lights[i];

        if (!light.enabled) continue;
        ui.set_properties(editor_light_ids + std::uint64_t(i) + 1u, light_properties(light));
    }
    for (const auto &cube : world->instances)
        if (cube.mesh_id == ::scene::unit_cube_mesh_id)
            ui.set_properties(editor_cube_ids + cube.id, cube_properties(cube));

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
        editor::instance().set_catalog({{"light", "Light", "Lighting"}, {"cube", "Cube", "Geometry"}});
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
