#include "srv/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/player/movement.hpp"
#include "srv/shared/scene/scene.hpp"
#include "weapons.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <csignal>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
    void quad(::scene::mesh& mesh, vk::util::math::vec3 a, vk::util::math::vec3 b, vk::util::math::vec3 c, vk::util::math::vec3 d, vk::util::math::vec3 normal)
    {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());

        mesh.vertices.push_back({a, normal, 0, 0});
        mesh.vertices.push_back({b, normal, 1, 0});
        mesh.vertices.push_back({c, normal, 1, 1});
        mesh.vertices.push_back({d, normal, 0, 1});

        mesh.indices.insert(mesh.indices.end(), {
            base, base + 1, base + 2,
            base, base + 2, base + 3,
        });
    }

    ::scene::mesh make_rect(std::uint32_t id, float x, float y, float z,
                            std::array<float, 4> color, float roughness = 0.85f)
    {
        ::scene::mesh mesh;
        mesh.id = id;
        mesh.surface.base_color = color;
        mesh.surface.roughness = roughness;
    
        quad(mesh, {-x, -y,  z}, { x, -y,  z}, { x,  y,  z}, {-x,  y,  z}, { 0,  0,  1});
        quad(mesh, { x, -y, -z}, {-x, -y, -z}, {-x,  y, -z}, { x,  y, -z}, { 0,  0, -1});
        quad(mesh, {-x, -y, -z}, {-x, -y,  z}, {-x,  y,  z}, {-x,  y, -z}, {-1,  0,  0});
        quad(mesh, { x, -y,  z}, { x, -y, -z}, { x,  y, -z}, { x,  y,  z}, { 1,  0,  0});
        quad(mesh, {-x,  y,  z}, { x,  y,  z}, { x,  y, -z}, {-x,  y, -z}, { 0,  1,  0});
        quad(mesh, {-x, -y, -z}, { x, -y, -z}, { x, -y,  z}, {-x, -y,  z}, { 0, -1,  0});
    
        return mesh;
    }

    // Procedural visual-only texture. The physical floor stays one mesh and
    // one authoritative AABB, so water collisions cannot acquire tile seams.
    // A 256 x 256 RGBA pattern is small enough to stream with the scene.
    ::scene::texture make_floor_pattern()
    {
        constexpr std::uint32_t width = 256;
        constexpr std::uint32_t tile_count = 20;
        ::scene::texture result{};
        result.id = 0xF1000001u;
        result.width = result.height = width;
        result.pixels.resize(std::size_t(width) * width * 4);
        for (std::uint32_t y = 0; y < width; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const auto tile_x = x * tile_count / width;
            const auto tile_y = y * tile_count / width;
            const auto local_x = (x * tile_count) % width;
            const auto local_y = (y * tile_count) % width;
            const bool inside = tile_x >= 5 && tile_x < 15 &&
                                tile_y >= 5 && tile_y < 15;
            const bool alternate = ((tile_x + tile_y) & 1u) != 0u;
            const bool seam = local_x < 13 || local_y < 13;
            const bool cross = inside && (tile_x == 10 || tile_y == 10) &&
                               (local_x < 26 || local_y < 26);
            std::array<std::uint8_t, 3> rgb{};
            if (seam)
                rgb = inside ?
                    std::array<std::uint8_t, 3>{28, 34, 39} :
                    std::array<std::uint8_t, 3>{42, 47, 51};
            else if (inside)
                rgb = alternate ?
                    std::array<std::uint8_t, 3>{139, 149, 149} :
                    std::array<std::uint8_t, 3>{57, 69, 77};
            else
                rgb = alternate ?
                    std::array<std::uint8_t, 3>{78, 85, 91} :
                    std::array<std::uint8_t, 3>{48, 55, 61};
            // Subtle warm navigation axes for identifying the floor beneath
            // clear water. These marks do not change any collision geometry.
            if (cross && !seam)
                rgb = {167, 145, 105};
            const std::size_t index =
                (std::size_t(y) * width + x) * 4;
            result.pixels[index + 0] = rgb[0];
            result.pixels[index + 1] = rgb[1];
            result.pixels[index + 2] = rgb[2];
            result.pixels[index + 3] = 255;
        }
        return result;
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
        settings.bind_host = "127.0.0.1";
        settings.port = 27015;
        settings.duration = 0;
        settings.headless = false;
        settings.help = false;
        game::options::init(argc, argv, game::options::mode::server_only,
                            std::move(settings));
        if (game::options::get()->help)
        {
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

        // One game-authored gun profile supplies both the server's
        // authoritative rules and the client's models, controls and clips.
        for(const auto& gun:game_weapons::all())
            srv::application::add_weapon(gun.rules);
        srv::application::on_weapon_command(
            [](srv::application::weapon_command_context& event)
            {
                const auto origin = event.player.position;
                const auto direction = weapon::direction(event.command.yaw, event.command.pitch);

                const auto world_hit = event.world.trace(
                    origin, direction, event.definition.range
                );
                
                const auto distance = [&](const weapon::hit& h) {
                    return h.impacted
                        ? vk::util::math::length_squared(h.position - origin)
                        : std::numeric_limits<float>::infinity();
                };
                
                return world_hit;
            }
        );

        net::socket::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        ::scene::state initial_scene;
        // High-contrast, neutral-toned fluid laboratory. The tiled pattern
        // provides optical reference detail beneath clear water. Physics
        // still sees one continuous floor with its upper face exactly y=0.
        initial_scene.textures.push_back(make_floor_pattern());
        auto floor = make_rect(1, 10.0f, 0.1f, 10.0f,
                               {1.f, 1.f, 1.f, 1.f}, .78f);
        floor.surface.base_color_texture = initial_scene.textures.back().id;
        auto cube = make_rect(2, .5f, .5f, .5f,
                              {.31f, .33f, .36f, 1.f}, .62f);
        auto wall_x = make_rect(3, .34f, 2.f, 5.f,
                                {.19f, .23f, .27f, 1.f}, .88f);
        auto wall_z = make_rect(4, 5.f, 2.f, .34f,
                                {.32f, .30f, .27f, 1.f}, .88f);
        auto central_cube = make_rect(5, .5f, .5f, .5f,
                                     {.52f, .38f, .26f, 1.f}, .53f);
        // Thin architectural highlight bands are purely visual. Do not
        // add them to the server's collision-box list or fluid solver.
        auto trim_x = make_rect(6, .351f, .045f, 5.01f,
                                {.49f, .50f, .48f, 1.f}, .7f);
        auto trim_z = make_rect(7, 5.01f, .045f, .351f,
                                {.49f, .50f, .48f, 1.f}, .7f);
        initial_scene.meshes.push_back(std::move(floor));
        initial_scene.meshes.push_back(std::move(cube));
        initial_scene.meshes.push_back(std::move(wall_x));
        initial_scene.meshes.push_back(std::move(wall_z));
        initial_scene.meshes.push_back(std::move(central_cube));
        initial_scene.meshes.push_back(std::move(trim_x));
        initial_scene.meshes.push_back(std::move(trim_z));

        auto add = [&](std::uint32_t id, std::uint32_t mesh, vk::util::math::vec3 position)
        {
            ::scene::instance object{};
            object.id = id;
            object.mesh_id = mesh;
            object.pose.position = position;

            // One authoritative definition for both the rendered object and
            // its fluid collision box. Transform the actual mesh vertices,
            // rather than repeating hard-coded dimensions in client.cpp.
            const auto geometry = std::find_if(
                initial_scene.meshes.begin(), initial_scene.meshes.end(),
                [mesh](const ::scene::mesh& item) { return item.id == mesh; });
            if (geometry == initial_scene.meshes.end())
                throw std::runtime_error("No mesh for fluid collision bounds");

            ::scene::collision_box bounds{};
            const auto infinity = std::numeric_limits<float>::infinity();
            bounds.minimum = { infinity, infinity, infinity };
            bounds.maximum = {-infinity,-infinity,-infinity };
            for (const auto& vertex : geometry->vertices)
            {
                const auto& p = vertex.position;
                const auto& scale = object.pose.scale;
                const auto local = vk::util::math::vec3{
                    p.x * scale.x, p.y * scale.y, p.z * scale.z
                };
                const auto rotated = vk::util::math::rotate(
                    object.pose.rotation, local);
                const auto world = position + rotated;
                bounds.minimum.x = std::min(bounds.minimum.x, world.x);
                bounds.minimum.y = std::min(bounds.minimum.y, world.y);
                bounds.minimum.z = std::min(bounds.minimum.z, world.z);
                bounds.maximum.x = std::max(bounds.maximum.x, world.x);
                bounds.maximum.y = std::max(bounds.maximum.y, world.y);
                bounds.maximum.z = std::max(bounds.maximum.z, world.z);
            }
            initial_scene.instances.push_back(object);
            initial_scene.collision_boxes.push_back(bounds);
        };
        add(1, 1, {0, -0.1f, 0});
        add(2, 2, {-2.f, .5f, 0});
        add(3, 5, {0, .5f, 0});
        add(4, 2, {2.f, .5f, 0});
        add(5, 3, {5.0f, 2.0f, 0});
        add(6, 3, {-5.0f, 2.0f, 0});
        add(7, 4, {0, 2.0f, 5.0f});
        add(8, 4, {0, 2.0f, -5.0f});
        // Wall detail is decoration only; keeps physics identical to the
        // original eight authoritative room and obstacle colliders.
        auto add_detail = [&](std::uint32_t id, std::uint32_t mesh,
                              vk::util::math::vec3 position)
        {
            ::scene::instance object{};
            object.id = id;
            object.mesh_id = mesh;
            object.pose.position = position;
            initial_scene.instances.push_back(object);
        };
        add_detail(9, 6, {5.f, 2.65f, 0.f});
        add_detail(10, 6, {-5.f, 2.65f, 0.f});
        add_detail(11, 7, {0.f, 2.65f, 5.f});
        add_detail(12, 7, {0.f, 2.65f, -5.f});
        // Restore the original seated mermaid above the middle cube.
        const auto& center_cube = initial_scene.instances.at(2);
        ::scene::transform mermaid = center_cube.pose;
        mermaid.position.y += 0.5f;
        mermaid.position.z += 0.5f / 3.f;
        mermaid.scale = {1.5f, 1.5f, 1.5f};
        srv::application::load_model("assets/mermaid/mermaid.glb", mermaid, 0);

        // Fluid collision boxes are generated by add() and streamed with the scene.
        srv::application::set_scene(std::move(initial_scene));

        // Game-owned asset declarations; imported meshes and animations stream to clients.
        // ::scene::transform hallway{};
        // hallway.position = {0, 0, 0};
        // hallway.rotation = {0, 0, 0, 1};
        // hallway.scale = {1, 1, 1};
        // srv::application::load_model("../MODELS/hospital_hallway.glb", hallway, 0);

        srv::application::init();
        const int result = srv::application::run();
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        return result;
    }
    catch (const std::exception& e)
    {
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-server: " << e.what() << '\n';
        return 1;
    }
}
