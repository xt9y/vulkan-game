#pragma once
#include "camera.hpp"
#include "connection.hpp"
#include "scene_renderer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <editor/vulkan.hpp>
#include <iterator>
#include <memory>
#include <pcloud/pcloud.hpp>
#include <optics/optics.hpp>
#include <optics/caustics.hpp>
#include <pmap/pmap.hpp>
#include <optional>
#include <vk/vk.hpp>

namespace clt::postprocess
{
namespace {
std::unique_ptr<optics::compositor> material_compositor;
std::unique_ptr<optics::caustics> photon_caustics;
std::unique_ptr<pm::mapper> photon_transport;
}
void create();
void synchronize_scene();
void release();
void present(vk::CommandBuffer command, std::uint32_t image, const camera::frame &view);

namespace
{
void append_remote_players(editor::draw_list &commands, const camera::frame &view, vk::Extent2D extent)
{
    if (!editor::instance().visible() || !extent.width || !extent.height) return;

    const auto parameters = connection::movement_parameters();
    if (!parameters) return;

    const auto players = connection::remote_players();

    if (players.empty()) return;
    using namespace vk::util::math;

    const mat4 vp = perspective(view.pose.fov, float(extent.width) / float(extent.height), .1f, 1000.f) * view.view;

    struct screen_point
    {
        float x = 0, y = 0;
        bool valid = false;
    };

    for (const auto &player : players)
    {
        const vec3 lower = player.position - vec3{parameters->radius, 0.f, parameters->radius};
        const vec3 upper = player.position + vec3{parameters->radius, parameters->height, parameters->radius};

        std::array<screen_point, 8> projected{};

        for (int corner = 0; corner < 8; ++corner)
        {
            const vec3 position{
                (corner & 1) ? upper.x : lower.x,
                (corner & 2) ? upper.y : lower.y,
                (corner & 4) ? upper.z : lower.z
            };

            const vec4 clip = vp * vec4{position, 1.f};

            if (!std::isfinite(clip.w) || clip.w <= .0001f || clip.z < 0.f || clip.z > clip.w) continue;

            const float x = 480.f * (1.f + clip.x / clip.w);
            const float y = 270.f * (1.f + clip.y / clip.w);

            if (std::isfinite(x) && std::isfinite(y)) projected[corner] = {x, y, true};
        }

        for (int corner = 0; corner < 8; ++corner)
            for (int axis = 0; axis < 3; ++axis)
            {
                if (corner & (1 << axis)) continue;
                const auto &a = projected[corner];
                const auto &b = projected[corner | (1 << axis)];

                if (!a.valid || !b.valid) continue;

                const float dx = b.x - a.x, dy = b.y - a.y;
                float start = 0.f, end = 1.f;
                const auto clip = [&](float p, float q)
                {
                    if (std::abs(p) < 1e-7f) return q >= 0.f;

                    const float t = q / p;

                    if (p < 0.f) start = std::max(start, t);
                    else
                        end = std::min(end, t);

                    return start <= end;
                };

                if (!clip(-dx, a.x) || !clip(dx, 960.f - a.x) ||
                    !clip(-dy, a.y) || !clip(dy, 540.f - a.y)) continue;
                const float x0 = a.x + dx * start, y0 = a.y + dy * start;
                const float x1 = a.x + dx * end, y1 = a.y + dy * end;
                const int steps = std::max(1, int(std::ceil(
                                                  std::max(std::abs(x1 - x0), std::abs(y1 - y0)) / 2.f)));
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = float(i) / float(steps);

                    editor::draw_command pixel{};
                    pixel.x = int(std::lround(x0 + (x1 - x0) * t)) - 1;
                    pixel.y = int(std::lround(y0 + (y1 - y0) * t)) - 1;
                    pixel.width = pixel.height = 2;
                    pixel.tint = {0.f, 1.f, 0.2f, 1.f};
                    commands.push_back(std::move(pixel));
                }
            }
    }
}

std::shared_ptr<const ::scene::state> registered;
constexpr float bounce_reach = 2.6f;
bool lighting_unchanged(const ::scene::state &previous, const ::scene::state &incoming)
{
    if (!::scene::geometry_prefix_unchanged(previous, incoming, true) || incoming.lights.size() != previous.lights.size() ||
        incoming.collision_boxes.size() != previous.collision_boxes.size() || incoming.skins.size() != previous.skins.size())
        return false;

    for (std::size_t i = 0; i < previous.lights.size(); ++i)
    {
        const auto &a = previous.lights[i], &b = incoming.lights[i];

        if (a.position.x != b.position.x || a.position.y != b.position.y || a.position.z != b.position.z || a.radius != b.radius || a.color.x != b.color.x || a.color.y != b.color.y || a.color.z != b.color.z || a.strength != b.strength)
            return false;
    }

    for (std::size_t i = 0; i < previous.collision_boxes.size(); ++i)
    {
        const auto &a = previous.collision_boxes[i], &b = incoming.collision_boxes[i];

        if (a.minimum.x != b.minimum.x || a.minimum.y != b.minimum.y || a.minimum.z != b.minimum.z || a.maximum.x != b.maximum.x || a.maximum.y != b.maximum.y || a.maximum.z != b.maximum.z)
            return false;
    }

    using namespace vk::util::math;

    for (std::size_t i = previous.instances.size(); i < incoming.instances.size(); ++i)
    {
        const auto &instance = incoming.instances[i];

        const auto found = std::find_if(incoming.meshes.begin(), incoming.meshes.end(), [&](const ::scene::mesh &mesh)
                                        { return mesh.id == instance.mesh_id; });
        if (found == incoming.meshes.end()) return false;
        if (found->surface.alpha_mode == 2 || found->indices.empty()) continue;
        if (found->skin_id) return false;

        float furthest_squared = 0.f;

        for (const auto &vertex : found->vertices)
            furthest_squared = std::max(furthest_squared, length_squared(vertex.position));

        const float scale = std::max({std::abs(instance.pose.scale.x), std::abs(instance.pose.scale.y), std::abs(instance.pose.scale.z)});

        const auto &rotation = instance.pose.rotation;

        const float rotation_length_squared = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;

        if (!std::isfinite(scale) || !std::isfinite(furthest_squared) || !std::isfinite(rotation_length_squared) || std::abs(rotation_length_squared - 1.f) > 0.00001f)
            return false;

        const float radius = std::sqrt(furthest_squared) * scale + 0.01f;

        for (const auto &light : incoming.lights)
        {
            const float reach = radius + light.radius + bounce_reach;

            if (length_squared(instance.pose.position - light.position) <= reach * reach)
                return false;
        }
    }

    return true;
}

std::shared_ptr<pc::world> prepare_world(const ::scene::state *source)
{
    auto prepared = std::make_shared<pc::world>();

    if (!source) return prepared;
    using namespace vk::util::math;
    prepared->lights.reserve(source->lights.size());

    for (const auto &light : source->lights)
        prepared->lights.push_back({light.position, light.radius, light.color, light.enabled ? light.strength : 0.f});
    prepared->collision_boxes.reserve(source->collision_boxes.size());

    for (const auto &box : source->collision_boxes)
        prepared->collision_boxes.push_back({box.minimum, box.maximum});
    std::unordered_map<std::uint32_t, const ::scene::mesh *> mesh_lookup;
    mesh_lookup.reserve(source->meshes.size());

    for (const auto &mesh : source->meshes)
        mesh_lookup.emplace(mesh.id, &mesh);

    for (const auto &instance : source->instances)
    {
        const auto found = mesh_lookup.find(instance.mesh_id);

        if (found == mesh_lookup.end()) continue;

        const auto *mesh = found->second;

        if (mesh->surface.alpha_mode == 2) continue;
        std::vector<animation::matrix> palette;

        if (mesh->skin_id && !animation::sample(mesh->skin_id, instance.animation, 0.0f, palette, instance.overlay_animation, instance.overlay_weight, instance.overlay_root))
            throw std::runtime_error("Cannot sample imported shadow caster skin");

        const auto transformed = [&](const ::scene::vertex &v)
        {
            vec3 local = v.position;

            if (!palette.empty()) {
                local = {};

                for (std::size_t j = 0; j < 4; ++j)
                {
                    if (v.weights[j] <= 0.0f) continue;
                    if (v.joints[j] >= palette.size())
                        throw std::runtime_error("Shadow caster joint index invalid");
                    const auto &m = palette[v.joints[j]];

                    const vec3 p = v.position;

                    local = local + vec3{
                                        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]
                                    } * v.weights[j];
                }
            }

            const auto &scale = instance.pose.scale;

            return instance.pose.position + rotate(instance.pose.rotation, {local.x * scale.x, local.y * scale.y, local.z * scale.z});
        };

        std::vector<vec3> positions;
        positions.reserve(mesh->vertices.size());

        for (const auto &vertex : mesh->vertices)
            positions.push_back(transformed(vertex));

        if (instance.mesh_id == ::scene::unit_cube_mesh_id)
            for (std::size_t i = 0; i + 3 < positions.size(); i += 4) {
                const auto base = positions[i];
                const auto u = positions[i + 1] - base;
                const auto v = positions[i + 3] - base;
                prepared->surfaces.push_back({base, u, v, normalize(cross(u, v))});
            }

        for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
        {
            const vec3 a = positions.at(mesh->indices[i]);
            const vec3 b = positions.at(mesh->indices[i + 1]);
            const vec3 c = positions.at(mesh->indices[i + 2]);

            if (length_squared(cross(b - a, c - a)) < 1e-12f) continue;
            if (!pc::relevant_triangle({a, b, c}, prepared->lights))
                continue;
            prepared->triangles.push_back({a, b, c});
        }
    }

    return prepared;
}
// Convert the authoritative scene into GPU photon-tracing surfaces.
// The transport-only mapper uses the full software BVH but does not allocate
// a per-face irradiance atlas or run a fullscreen ray tracer.
std::optional<pm::scene> photon_scene(const ::scene::state& world)
{
    using namespace vk::util::math;
    const auto light=std::find_if(world.lights.begin(),world.lights.end(),
        [](const auto& l){return l.enabled && l.strength>0.f;});
    if(light==world.lights.end())return std::nullopt;
    pm::scene result{};
    for(const auto& inst:world.instances) {
        const auto it=std::find_if(world.meshes.begin(),world.meshes.end(),
            [&](const auto& mesh){return mesh.id==inst.mesh_id;});
        if(it==world.meshes.end() || it->surface.alpha_mode==2)continue;
        const auto& mesh=*it;
        const auto transform=[&](vec3 p) {
            const auto& scale=inst.pose.scale;
            return inst.pose.position+rotate(inst.pose.rotation,
                {p.x*scale.x,p.y*scale.y,p.z*scale.z});
        };
        for(std::size_t i=0;i+2<mesh.indices.size();i+=3) {
            const auto ia=mesh.indices[i],ib=mesh.indices[i+1],ic=mesh.indices[i+2];
            if(ia>=mesh.vertices.size() || ib>=mesh.vertices.size() ||
               ic>=mesh.vertices.size())continue;
            const vec3 a=transform(mesh.vertices[ia].position);
            const vec3 u=transform(mesh.vertices[ib].position)-a;
            const vec3 v=transform(mesh.vertices[ic].position)-a;
            const auto n=cross(u,v);
            if(!std::isfinite(n.x) || !std::isfinite(n.y) ||
               !std::isfinite(n.z) || length_squared(n)<=1e-8f)continue;
            pm::surface surface{};
            surface.origin=a;surface.u=u;surface.v=v;
            surface.normal=normalize(n);
            surface.albedo={
                std::clamp(mesh.surface.base_color[0]*inst.tint.x,0.f,1.f),
                std::clamp(mesh.surface.base_color[1]*inst.tint.y,0.f,1.f),
                std::clamp(mesh.surface.base_color[2]*inst.tint.z,0.f,1.f)
            };
            surface.triangle=true;
            if(inst.pattern==2u || inst.pattern==3u) {
                surface.optical_type=inst.pattern==2u?1u:2u;
                surface.optical_strength=inst.optical_strength;
                surface.roughness=inst.optical_mattness;
            }
            result.surfaces.push_back(surface);
        }
    }
    pm::surface emitter{};
    emitter.origin=light->position+vec3{-.2f,0.f,-.2f};
    emitter.u={.4f,0.f,0.f};
    emitter.v={0.f,0.f,.4f};
    emitter.normal={0.f,-1.f,0.f};
    emitter.emission=light->color*std::max(light->strength,0.f)*6.f;
    emitter.albedo={0.f,0.f,0.f};
    result.emitter=std::uint32_t(result.surfaces.size());
    result.surfaces.push_back(emitter);
    return result;
}
} // namespace
void create()
{
    std::vector<pc::gbuffer> gbuffers;

    const auto count = static_cast<std::uint32_t>(vk::swapchain::get()->images.size());
    gbuffers.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i)
        gbuffers.push_back(scene_renderer::outputs(i));
    pc::configuration shaders{};
    const auto &configured = game::options::get()->graphics;
    shaders.lighting_shader = configured.lighting_shader;
    shaders.tonemap_shader = configured.tonemap_shader;
    shaders.fullscreen_shader = configured.fullscreen_shader;
    shaders.presentation_shader = configured.presentation_shader;
    pc::create(gbuffers, shaders);
    std::vector<vk::ImageView> depth_views;
    depth_views.reserve(count);
    for(std::uint32_t i=0;i<count;++i)
        depth_views.push_back(scene_renderer::outputs(i)[3]);
    auto caustics=std::make_unique<optics::caustics>();
    caustics->create(vk::swapchain::get()->extent,depth_views);
    std::vector<optics::input> material_frames;
    material_frames.reserve(count);
    for(std::uint32_t i=0;i<count;++i) {
        const auto ordinary=scene_renderer::outputs(i);
        const auto optical=scene_renderer::optical_outputs(i);
        material_frames.push_back({
            pc::ldr_view(i),ordinary[3],optical[0],optical[1],
            optical[2],optical[3],caustics->buffer(i)
        });
    }
    auto compositor=std::make_unique<optics::compositor>();
    compositor->create(pc::presentation_pass(),material_frames);
    material_compositor=std::move(compositor);
    photon_caustics=std::move(caustics);
}

void synchronize_scene()
{
    const auto source = connection::snapshot();

    if (source == registered) return;
    if (!(source && registered && lighting_unchanged(*registered, *source))) {
        pc::set_scene(prepare_world(source.get()));
        pc::synchronize_scene();
    }
    if(photon_caustics)photon_caustics->set_photons({});
    photon_transport.reset();
    if(source && std::any_of(source->instances.begin(),source->instances.end(),
        [](const auto& inst){return inst.pattern==2u || inst.pattern==3u;})) {
        auto scene=photon_scene(*source);
        if(scene) {
            pm::configuration config{};
            config.transport_only=true;
            config.photons=4096;
            config.gi_updates=32;
            config.bounces=4;
            config.atlas_tile=32;
            auto mapper=std::make_unique<pm::mapper>();
            mapper->create(*scene,{1,1},config);
            const auto buffers=mapper->photon_buffers();
            photon_caustics->set_photons({buffers.sorted,buffers.used,buffers.capacity});
            photon_transport=std::move(mapper);
        }
    }
    registered=source;
}

void release()
{
    material_compositor.reset();
    photon_caustics.reset();
    photon_transport.reset();
    pc::release();
    registered.reset();
}

void update_photons(vk::CommandBuffer cmd,std::uint32_t image,
                    const camera::frame& view)
{
    if(!photon_caustics || !scene_renderer::has_optics())return;
    if(photon_transport)photon_transport->record_transport(cmd);
    const auto extent=vk::swapchain::get()->extent;
    const auto projection=vk::util::math::perspective(
        view.pose.fov,float(extent.width)/float(extent.height),.1f,1000.f)*view.view;
    photon_caustics->record(cmd,image,projection);
}

void present(vk::CommandBuffer cmd, std::uint32_t image, const camera::frame &view)
{
    pc::present(cmd, image, [view,image](vk::CommandBuffer command, vk::Extent2D extent)
                {
            if(material_compositor && scene_renderer::has_optics()) {
                using namespace vk::util::math;
                const float half_tan=std::tan(view.pose.fov*.5f);
                const auto& v=view.view;
                optics::camera params{};
                params.view_projection=perspective(view.pose.fov,
                    float(extent.width)/float(extent.height),.1f,1000.f)*v;
                params.eye={view.eye.x,view.eye.y,view.eye.z,1.f};
                params.right={v(0,0),v(0,1),v(0,2),
                              half_tan*float(extent.width)/float(extent.height)};
                params.up={v(1,0),v(1,1),v(1,2),half_tan};
                params.forward={-v(2,0),-v(2,1),-v(2,2),0.f};
                material_compositor->record(command,image,params);
            }
            editor::draw_list commands;
            append_remote_players(commands,view,extent);
            auto ui=editor::instance().paint(int(extent.width),int(extent.height));
            commands.insert(commands.end(),std::make_move_iterator(ui.begin()),std::make_move_iterator(ui.end()));
            editor::vulkan::record(command,commands,extent); });
}

} // namespace clt::postprocess
