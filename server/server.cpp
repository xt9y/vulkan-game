#include "game/application.hpp"
#include "game/options.hpp"
#include "game_weapons.hpp"
#include "srv/application.hpp"
#include <csignal>
#include <iostream>

static ::scene::mesh box_mesh(std::uint32_t id, vk::util::math::vec3 h, bool checker = false)
{
    using vk::util::math::vec3;
    ::scene::mesh mesh{};
    mesh.id = id;
    mesh.surface.base_color = {1, 1, 1, 1};
    mesh.surface.roughness = .85f;
    mesh.surface.procedural_pattern = checker ? 1u : 0u;

    const vec3 p[8] = {
        {-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, h.y, -h.z}, {-h.x, h.y, -h.z}, {-h.x, -h.y, h.z}, {h.x, -h.y, h.z}, {h.x, h.y, h.z}, {-h.x, h.y, h.z}
    };

    const int faces[6][4] = {
        {4, 5, 6, 7}, {1, 0, 3, 2}, {0, 4, 7, 3}, {5, 1, 2, 6}, {7, 6, 2, 3}, {0, 1, 5, 4}
    };

    const vec3 normals[6] = {
        {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}
    };

    for (std::uint32_t f = 0; f < 6; ++f)
    {
        const auto start = std::uint32_t(mesh.vertices.size());

        for (int j = 0; j < 4; ++j)
            mesh.vertices.push_back({p[faces[f][j]], normals[f], float(j == 1 || j == 2), float(j >= 2)});
        mesh.indices.insert(mesh.indices.end(), {start, start + 1, start + 2, start, start + 2, start + 3});
    }

    return mesh;
}

// Unit-diameter UV sphere with interpolated normals for smooth primary and
// secondary reflections. Geometry is shared between inserted instances.
static ::scene::mesh sphere_mesh(std::uint32_t id)
{
    constexpr std::uint32_t rings=48,segments=96;
    constexpr float pi=3.14159265358979323846f;
    ::scene::mesh mesh{};
    mesh.id=id;
    mesh.surface.base_color={1,1,1,1};
    mesh.surface.roughness=.85f;
    for(std::uint32_t j=0;j<=rings;++j) {
        const float theta=pi*float(j)/float(rings);
        for(std::uint32_t i=0;i<=segments;++i) {
            const float phi=2.f*pi*float(i)/float(segments);
            const vk::util::math::vec3 n{
                std::sin(theta)*std::cos(phi),
                std::cos(theta),
                std::sin(theta)*std::sin(phi)
            };
            mesh.vertices.push_back({n*.5f,n,float(i)/segments,float(j)/rings});
        }
    }
    for(std::uint32_t j=0;j<rings;++j)
        for(std::uint32_t i=0;i<segments;++i) {
            const auto a=j*(segments+1)+i;
            const auto b=a+segments+1;
            mesh.indices.insert(mesh.indices.end(),{a,a+1,b,a+1,b+1,b});
        }
    return mesh;
}

static ::scene::state make_level()
{
    using vk::util::math::vec3;
    ::scene::state world{};
    world.meshes.push_back(box_mesh(1, {10, .1f, 10}, true));
    world.meshes.push_back(box_mesh(2, {.5f, .5f, .5f}));
    world.meshes.push_back(sphere_mesh(::scene::unit_sphere_mesh_id));

    const auto place = [&](std::uint32_t id, std::uint32_t mesh, vec3 pos)
    {
        ::scene::instance instance{};
        instance.id = id;
        instance.mesh_id = mesh;
        instance.pose.position = pos;
        world.instances.push_back(instance);
    };

    place(1, 1, {0, -.1f, 0});
    // Static room probe surface; editable cubes supply transformed probe faces.
    // PCloud derives the floor's occlusion box from its live mesh pose.
    // Left wall: thin, full-height perfect mirror facing into the room.
    // Unit cube geometry makes these the exact inspector dimensions.
    {
        ::scene::instance mirror{};
        mirror.id=2;
        mirror.mesh_id=::scene::unit_cube_mesh_id;
        mirror.pose.position={-2.f,3.f,0.f};
        mirror.pose.scale={.10f,6.f,5.f};
        mirror.pattern=3u;
        mirror.optical_strength=1.f;
        mirror.optical_mattness=0.f;
        world.instances.push_back(mirror);
    }
    place(3, 2, {0, .5f, 0});
    place(4, 2, {2, .5f, 0});
    ::scene::transform mermaid{};
    mermaid.position = {0, 1, .5f / 3.f};
    mermaid.scale = {1.5f, 1.5f, 1.5f};
    srv::application::load_model("assets/mermaid/mermaid.glb", mermaid, 0);
    world.lights.push_back({{0, 3, 1}, 6, {1, .5f, .2f}, 4});

    return world;
}

static void stop(int)
{
    game::application::get()->stop.store(true, std::memory_order_relaxed);
}

int main(int argc, char **argv)
{
    try
    {
        game::options::state settings{};
        settings.bind_host = "0.0.0.0";
        settings.port = 27015;
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
    }

    catch (const std::exception &e)
    {
        srv::application::shutdown();
        net::socket::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-server: " << e.what() << '\n';

        return 1;
    }
}
