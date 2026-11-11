#pragma once
#include "camera.hpp"
#include "connection.hpp"
#include "scene_renderer.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <editor/vulkan.hpp>
#include <iterator>
#include <memory>
#include <gpu/timestamps.hpp>
#include <pcloud/pcloud.hpp>
#include <optics/optics.hpp>
#include <pmap/pmap.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <climits>
#include <stb_image.h>
#include <vk/vk.hpp>

namespace clt::postprocess
{
namespace {
std::unique_ptr<optics::compositor> material_compositor;
std::unique_ptr<pm::mapper> photon_transport;
bool transport_pending=false;
struct skinned_face {
    std::uint32_t face=0;
    const ::scene::instance* instance=nullptr;
    const ::scene::mesh* mesh=nullptr;
    std::array<std::uint32_t,3> indices{};
};
std::vector<skinned_face> skinned_faces;
}
void create();
void synchronize_scene();
void release();
void update_skinned(std::uint32_t frame,const camera::frame& view);
void present(vk::CommandBuffer command, std::uint32_t image,
             const camera::frame &view,const gpu::timestamps* timings);

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
std::shared_ptr<const ::scene::state> optical_geometry_source;
constexpr float bounce_reach = 2.6f;
// Compare only inputs that PCloud actually consumes. Editor tint, procedural
// texture, mirror reflectivity and presentation metadata do not change its
// triangle positions, shadow casters or probe positions.
bool lighting_shape_unchanged(const ::scene::state& previous,
                              const ::scene::state& incoming)
{
    if(previous.atlas_revision!=incoming.atlas_revision ||
       previous.meshes.size()!=incoming.meshes.size() ||
       previous.skins.size()!=incoming.skins.size() ||
       previous.lights.size()!=incoming.lights.size() ||
       previous.collision_boxes.size()!=incoming.collision_boxes.size() ||
       previous.instances.size()>incoming.instances.size())
        return false;
    const auto active=[](const ::scene::point_light& l) {
        return l.enabled && l.strength>0.f;
    };
    for(std::size_t i=0;i<previous.lights.size();++i) {
        const auto& a=previous.lights[i];
        const auto& b=incoming.lights[i];
        if(a.position.x!=b.position.x || a.position.y!=b.position.y ||
           a.position.z!=b.position.z || a.radius!=b.radius ||
           active(a)!=active(b))
            return false;
    }
    for(std::size_t i=0;i<previous.collision_boxes.size();++i) {
        const auto& a=previous.collision_boxes[i];
        const auto& b=incoming.collision_boxes[i];
        if(a.minimum.x!=b.minimum.x || a.minimum.y!=b.minimum.y ||
           a.minimum.z!=b.minimum.z || a.maximum.x!=b.maximum.x ||
           a.maximum.y!=b.maximum.y || a.maximum.z!=b.maximum.z)
            return false;
    }
    std::unordered_map<std::uint32_t,const ::scene::mesh*> meshes;
    meshes.reserve(incoming.meshes.size());
    for(const auto& mesh:incoming.meshes)meshes.emplace(mesh.id,&mesh);
    const auto caster=[&](const ::scene::instance& i) {
        const auto it=meshes.find(i.mesh_id);
        if(it==meshes.end() || it->second->surface.alpha_mode==2 ||
           i.pattern==2u)return false;
        const auto& mesh=*it->second;
        // PCloud also derives ground collision bounds and cube probe faces
        // from raw vertices, even if their index lists are empty.
        return !mesh.indices.empty() ||
               (i.mesh_id==::scene::ground_mesh_id && !mesh.vertices.empty()) ||
               (i.mesh_id==::scene::unit_cube_mesh_id &&
                mesh.vertices.size()>=4);
    };
    for(std::size_t i=0;i<previous.instances.size();++i) {
        const auto& a=previous.instances[i];
        const auto& b=incoming.instances[i];
        if(a.id!=b.id || a.mesh_id!=b.mesh_id)return false;
        const bool was=caster(a),is=caster(b);
        if(was!=is)return false;
        if(!is)continue;
        if(!::scene::same_pose(a,b))return false;
        const auto* mesh=meshes.at(b.mesh_id);
        if(mesh->skin_id &&
           (a.animation!=b.animation ||
            a.overlay_animation!=b.overlay_animation ||
            a.overlay_root!=b.overlay_root ||
            a.overlay_weight!=b.overlay_weight))
            return false;
    }
    for(std::size_t i=previous.instances.size();i<incoming.instances.size();++i)
        if(caster(incoming.instances[i]))return false;
    return true;
}
bool lighting_unchanged(const ::scene::state& previous,
                        const ::scene::state& incoming)
{
    if(!lighting_shape_unchanged(previous,incoming))return false;
    for(std::size_t i=0;i<previous.lights.size();++i) {
        const auto& a=previous.lights[i];
        const auto& b=incoming.lights[i];
        if(a.color.x!=b.color.x || a.color.y!=b.color.y ||
           a.color.z!=b.color.z || a.strength!=b.strength)
            return false;
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

        // Transparent glass is handled by photonic transmission, not
        // opaque point-cloud shadow blocking.
        if (mesh->surface.alpha_mode == 2 || instance.pattern == 2u) continue;
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

        // Preserve the original ground-box occlusion exactly at its
        // default transform, but follow edits and disappear on deletion.
        if(instance.mesh_id==::scene::ground_mesh_id && !positions.empty()) {
            vec3 minimum=positions.front(),maximum=positions.front();
            for(const auto& p:positions) {
                minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y),
                         std::min(minimum.z,p.z)};
                maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y),
                         std::max(maximum.z,p.z)};
            }
            prepared->collision_boxes.push_back({minimum,maximum});
        }

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
// The transport-only mapper owns the same BVH used by the raster scene.
// Face UVs and vertex normals retain exact BVH face ordering.
std::optional<pm::scene> photon_scene(const ::scene::state& world,
                                       std::vector<optics::face_uv>& faces,
                                       std::vector<optics::face_normals>& normals)
{
    using namespace vk::util::math;
    const auto light=std::find_if(world.lights.begin(),world.lights.end(),
        [](const auto& l){return l.enabled && l.strength>0.f;});
    // Optical geometry must exist even in a dark scene: reflecting the
    // world is not conditional on a point light being enabled.
    pm::scene result{};
    faces.clear();
    normals.clear();
    skinned_faces.clear();
    result.omnidirectional_emitter=true;
    std::unordered_map<std::uint32_t,const ::scene::mesh*> meshes;
    meshes.reserve(world.meshes.size());
    for(const auto& mesh:world.meshes)meshes.emplace(mesh.id,&mesh);
    for(const auto& inst:world.instances) {
        const auto it=meshes.find(inst.mesh_id);
        if(it==meshes.end())continue;
        const auto& mesh=*it->second;
        // Match the raster pass's stable clip rule for blended hair cards.
        if(mesh.surface.alpha_mode==2 && !mesh.surface.base_color_texture &&
           mesh.surface.base_color[3]<.42f)continue;
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
            // Use PMap's actual triangle metric threshold. A second,
            // larger cross-product cutoff was discarding many small but
            // valid scaled Mermaid triangles before the reflection BVH.
            const float dot_uv=dot(u,v);
            const float metric=dot(u,u)*dot(v,v)-dot_uv*dot_uv;
            if(!std::isfinite(n.x) || !std::isfinite(n.y) ||
               !std::isfinite(n.z) || !(metric>1e-9f))continue;
            pm::surface surface{};
            surface.origin=a;surface.u=u;surface.v=v;
            surface.normal=normalize(n);
            surface.albedo={
                std::clamp(mesh.surface.base_color[0]*inst.tint.x,0.f,1.f),
                std::clamp(mesh.surface.base_color[1]*inst.tint.y,0.f,1.f),
                std::clamp(mesh.surface.base_color[2]*inst.tint.z,0.f,1.f)
            };
            surface.triangle=true;
            surface.emission={mesh.surface.emissive[0],mesh.surface.emissive[1],
                              mesh.surface.emissive[2]};
            surface.pattern=inst.pattern==1u?2u:
                (inst.pattern==UINT32_MAX &&
                 mesh.surface.procedural_pattern==1u?1u:0u);
            if(inst.pattern==2u || inst.pattern==3u) {
                surface.optical_type=inst.pattern==2u?1u:2u;
                // A mirror is perfectly reflective. Editor tint, strength
                // and mattness never reduce reflected world radiance.
                surface.optical_strength=inst.pattern==3u?
                    1.f:inst.optical_strength;
                surface.roughness=inst.pattern==3u?
                    0.f:inst.optical_mattness;
            }
            if(mesh.skin_id)
                skinned_faces.push_back({
                    std::uint32_t(result.surfaces.size()),&inst,&mesh,
                    {ia,ib,ic}
                });
            result.surfaces.push_back(surface);
            // Match the primary mesh vertex shader: inverse transpose
            // under nonuniform scaling, then rotate into world space.
            const auto smooth=[&](const ::scene::vertex& vertex) {
                const auto& scale=inst.pose.scale;
                vec3 n=rotate(inst.pose.rotation,{
                    vertex.normal.x/scale.x,
                    vertex.normal.y/scale.y,
                    vertex.normal.z/scale.z});
                if(!std::isfinite(n.x) || !std::isfinite(n.y) ||
                   !std::isfinite(n.z) || length_squared(n)<1e-8f)
                    n=surface.normal;
                else n=normalize(n);
                if(dot(n,surface.normal)<0.f)n=n*-1.f;
                return std::array<float,4>{n.x,n.y,n.z,0.f};
            };
            optics::face_normals shaded{
                smooth(mesh.vertices[ia]),
                smooth(mesh.vertices[ib]),
                smooth(mesh.vertices[ic])
            };
            shaded.a[3]=std::clamp(mesh.surface.metallic,0.f,1.f);
            shaded.b[3]=std::clamp(mesh.surface.roughness,.045f,1.f);
            shaded.c[3]=std::clamp(mesh.surface.occlusion,0.f,1.f);
            normals.push_back(shaded);
            optics::face_uv mapping{};
            mapping.a={mesh.vertices[ia].u,mesh.vertices[ia].v};
            mapping.b={mesh.vertices[ib].u,mesh.vertices[ib].v};
            mapping.c={mesh.vertices[ic].u,mesh.vertices[ic].v};
            mapping.texture_id=mesh.surface.base_color_texture;
            mapping.metallic_roughness_texture=
                mesh.surface.metallic_roughness_texture;
            mapping.normal_texture=mesh.surface.normal_texture;
            mapping.occlusion_texture=mesh.surface.occlusion_texture;
            mapping.emissive_texture=mesh.surface.emissive_texture;
            mapping.base_alpha=mesh.surface.base_color[3];
            mapping.alpha_cutoff=mesh.surface.alpha_mode==1 ?
                mesh.surface.alpha_cutoff :
                mesh.surface.alpha_mode==2 ? .42f : 0.f;
            faces.push_back(mapping);
        }
    }
    pm::surface emitter{};
    emitter.origin=light==world.lights.end()?vec3{}:light->position;
    emitter.u={.4f,0.f,0.f};
    emitter.v={0.f,0.f,.4f};
    emitter.normal={0.f,-1.f,0.f};
    emitter.emission=light==world.lights.end()?vec3{}:
        light->color*std::max(light->strength,0.f)*6.f;
    emitter.albedo={0.f,0.f,0.f};
    result.emitter=std::uint32_t(result.surfaces.size());
    result.surfaces.push_back(emitter);
    faces.emplace_back(); // The point-light emitter has no texture.
    normals.push_back({
        {0.f,-1.f,0.f,0.f},{0.f,-1.f,0.f,0.f},{0.f,-1.f,0.f,0.f}
    });
    return result;
}

std::vector<optics::texture_rgba> optical_textures(
    const ::scene::state& world,const std::vector<optics::face_uv>& faces)
{
    std::unordered_set<std::uint32_t> used;
    for(const auto& f:faces)
        for(const auto id:{f.texture_id,f.metallic_roughness_texture,
                           f.normal_texture,f.occlusion_texture,
                           f.emissive_texture})
            if(id)used.insert(id);
    std::vector<optics::texture_rgba> atlas;
    atlas.reserve(used.size());
    for(const auto& image:world.textures) {
        if(!used.erase(image.id))continue;
        const std::size_t bytes=std::size_t(image.width)*image.height*4u;
        if(!image.width || !image.height || bytes>256u*1024u*1024u)
            throw std::runtime_error("optics: invalid source texture dimensions");
        optics::texture_rgba entry{};
        entry.id=image.id;entry.width=image.width;entry.height=image.height;
        if(!image.pixels.empty()) {
            if(image.pixels.size()!=bytes)
                throw std::runtime_error("optics: invalid source texture bytes");
            entry.pixels=image.pixels;
        } else {
            if(image.encoded.size()>INT_MAX || image.encoded.empty())
                throw std::runtime_error("optics: missing encoded texture");
            int w=0,h=0,channels=0;
            std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> decoded(
                stbi_load_from_memory(image.encoded.data(),
                                      int(image.encoded.size()),
                                      &w,&h,&channels,4),stbi_image_free);
            if(!decoded || w!=int(image.width) || h!=int(image.height))
                throw std::runtime_error("optics: failed decoding reflected texture");
            entry.pixels.assign(decoded.get(),decoded.get()+bytes);
        }
        atlas.push_back(std::move(entry));
    }
    if(!used.empty())
        throw std::runtime_error("optics: BVH references missing streamed textures");
    return atlas;
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
    std::vector<pc::gbuffer> reflection_gbuffers;
    reflection_gbuffers.reserve(count);
    for(std::uint32_t i=0;i<count;++i)
        reflection_gbuffers.push_back(scene_renderer::reflection_outputs(i));
    pc::create_reflection(reflection_gbuffers);
    pc::set_exposure(configured.exposure);
    std::vector<optics::input> material_frames;
    material_frames.reserve(count);
    for(std::uint32_t i=0;i<count;++i) {
        const auto ordinary=scene_renderer::outputs(i);
        const auto optical=scene_renderer::optical_outputs(i);
        material_frames.push_back({
            pc::ldr_view(i),ordinary[3],optical[0],optical[1],
            optical[2],optical[3],pc::probe_view(i),pc::hdr_view(i),
            pc::reflection_ldr_view(i)
        });
    }
    auto compositor=std::make_unique<optics::compositor>();
    compositor->create(pc::presentation_pass(),material_frames);
    material_compositor=std::move(compositor);
}

// Reuse the GPU texture atlas when a network update changes only lights,
// object transforms or optical strengths. UV/texture contents and triangle
// ordering are the only data relevant to this atlas; uploaded materials
// and acceleration structures are handled separately by the mapper.
bool atlas_unchanged(const ::scene::state& a,const ::scene::state& b)
{
    if(a.atlas_revision!=b.atlas_revision ||
       a.instances.size()!=b.instances.size())return false;
    // Only instance-to-mesh ordering changes per-face UV mapping without
    // new asset payloads. Pose, tint and lighting cannot change atlas pixels.
    for(std::size_t i=0;i<a.instances.size();++i)
        if(a.instances[i].id!=b.instances[i].id ||
           a.instances[i].mesh_id!=b.instances[i].mesh_id)return false;
    return true;
}

// The optical BVH depends on actual triangles, instance material
// properties and the first photon emitter, not on unrelated light edits.
bool optical_geometry_unchanged(const ::scene::state& a,
                                const ::scene::state& b)
{
    if(a.atlas_revision!=b.atlas_revision ||
       a.meshes.size()!=b.meshes.size() ||
       a.skins.size()!=b.skins.size() ||
       a.instances.size()!=b.instances.size())
        return false;
    for(std::size_t i=0;i<a.instances.size();++i) {
        const auto& x=a.instances[i];
        const auto& y=b.instances[i];
        if(!::scene::same_geometry(x,y) ||
           x.animation!=y.animation ||
           x.animation_tick!=y.animation_tick ||
           x.overlay_animation!=y.overlay_animation ||
           x.overlay_root!=y.overlay_root ||
           x.overlay_weight!=y.overlay_weight)
            return false;
    }
    const auto emitter=[](const ::scene::state& v) {
        return std::find_if(v.lights.begin(),v.lights.end(),
            [](const auto& l){return l.enabled && l.strength>0.f;});
    };
    const auto x=emitter(a),y=emitter(b);
    if((x==a.lights.end())!=(y==b.lights.end()))return false;
    if(x!=a.lights.end() &&
       (x->position.x!=y->position.x ||
        x->position.y!=y->position.y ||
        x->position.z!=y->position.z))
        return false;
    return true;
}

void synchronize_scene()
{
    const auto source = connection::snapshot();

    if (source == registered) return;
    const bool reuse_atlas=source && registered &&
        atlas_unchanged(*registered,*source);
    const bool same_light_shape=source && registered &&
        lighting_shape_unchanged(*registered,*source);
    if(!same_light_shape) {
        pc::set_scene(prepare_world(source.get()));
        pc::synchronize_scene();
    } else if(!lighting_unchanged(*registered,*source)) {
        // The same shadow rays and probe positions, with new radiance.
        std::vector<pc::light> values;
        values.reserve(source->lights.size());
        for(const auto& l:source->lights)
            values.push_back({l.position,l.radius,l.color,
                              l.enabled?l.strength:0.f});
        pc::update_lights(values);
    }
    std::vector<optics::light> lights;
    if(source)
        for(const auto& light:source->lights)
            if(light.enabled && light.strength>0.f)
                lights.push_back({light.position,light.radius,
                                  light.color,light.strength});
    if(material_compositor) {
        // PCloud can recreate its 3D GI images when scene bounds change.
        // Refresh these descriptors after synchronization, not just at
        // compositor creation, or mirrors may sample a destroyed image.
        std::vector<vk::ImageView> probes;
        const auto frames=std::uint32_t(vk::swapchain::get()->images.size());
        probes.reserve(frames);
        for(std::uint32_t i=0;i<frames;++i)
            probes.push_back(pc::probe_view(i));
        material_compositor->set_probe_views(probes);
        material_compositor->set_probe_space(pc::probe_space());
    }
    const bool same_optical_geometry=source && registered &&
        photon_transport && optical_geometry_unchanged(*registered,*source);
    if(same_optical_geometry) {
        // No face, atlas, BVH, skin index, or emitted-ray origin changed.
        // Update light colors/intensity without allocating a second BVH.
        if(material_compositor)material_compositor->set_lights(lights);
        const auto first=std::find_if(source->lights.begin(),source->lights.end(),
            [](const auto& l){return l.enabled && l.strength>0.f;});
        const auto emission=first==source->lights.end() ?
            vk::util::math::vec3{} :
            first->color*std::max(first->strength,0.f)*6.f;
        const auto& world=photon_transport->current_scene();
        const auto old=world.surfaces[world.emitter].emission;
        if(old.x!=emission.x || old.y!=emission.y || old.z!=emission.z) {
            photon_transport->set_emission(world.emitter,emission);
            transport_pending=true;
        }
        registered=source;
        return;
    }
    // Prior GPU submissions have completed. New *geometry* requires new
    // photon traversal buffers; lighting-only changes never enter here.
    if(material_compositor)material_compositor->set_scene({},lights);
    photon_transport.reset();
    optical_geometry_source.reset();
    skinned_faces.clear();
    transport_pending=false;
    if(source && std::any_of(source->instances.begin(),source->instances.end(),
        [](const auto& inst){return inst.pattern==2u || inst.pattern==3u;})) {
        std::vector<optics::face_uv> face_uvs;
        std::vector<optics::face_normals> face_normals;
        auto scene=photon_scene(*source,face_uvs,face_normals);
        if(scene) {
            // skinned_faces stores pointers into this immutable state.
            optical_geometry_source=source;
            pm::configuration config{};
            config.transport_only=true;
            config.photons=8192; // Invisible indirect-lighting cache; no projected splats.
            config.gi_updates=1; // Stable photon map, reprojected from the camera every frame.
            config.bounces=4;
            config.atlas_tile=32;
            auto mapper=std::make_unique<pm::mapper>();
            mapper->create(*scene,{1,1},config);
            const auto g=mapper->geometry_buffers();
            if(material_compositor)
            {
                material_compositor->set_scene({
                    g.faces,g.nodes,g.triangles,g.scene_info,
                    g.photons,g.offsets,
                    std::uint32_t(scene->surfaces.size()),g.face_ids
                },lights,std::any_of(source->instances.begin(),
                    source->instances.end(),
                    [](const auto& inst){return inst.pattern==2u;}));
                material_compositor->set_normals(face_normals);
                if(!reuse_atlas)
                    material_compositor->set_textures(
                        face_uvs,optical_textures(*source,face_uvs));
            }
            photon_transport=std::move(mapper);
            transport_pending=true;
            std::clog << "optics: initialized photon transport with "
                      << scene->surfaces.size() << " surfaces and "
                      << config.photons << " emitted paths\n";
        }
    }
    registered=source;
}

void release()
{
    material_compositor.reset();
    photon_transport.reset();
    optical_geometry_source.reset();
    skinned_faces.clear();
    transport_pending=false;
    pc::release();
    registered.reset();
}

// Refit only skinned faces, using the *same* palette that was uploaded
// to mesh.vert this frame. CPU skinning happens once per unique vertex,
// then PMap refits the existing sorted BVH without changing its topology.
void update_skinned(std::uint32_t image,const camera::frame& view)
{
    if(!photon_transport || !material_compositor || skinned_faces.empty() ||
       scene_renderer::optical_rects(view).empty())
        return;
    const auto refit_start=std::chrono::steady_clock::now();
    using namespace vk::util::math;
    std::unordered_map<std::uint32_t,const std::vector<animation::matrix>*> poses;
    for(const auto& state:scene_renderer::skinning_poses())
        poses.emplace(state.id,&state.palette);
    if(poses.empty())return;
    struct transformed_vertex {vec3 position{},normal{};};
    std::unordered_map<std::uint32_t,std::vector<transformed_vertex>> vertices;
    std::vector<pm::geometry_update> surfaces;
    std::vector<optics::face_normal_update> normals;
    surfaces.reserve(skinned_faces.size());
    normals.reserve(skinned_faces.size());
    const auto& base=photon_transport->current_scene().surfaces;
    for(const auto& face:skinned_faces) {
        const auto pose=poses.find(face.instance->id);
        if(pose==poses.end() || face.face>=base.size())continue;
        auto [cache,created]=vertices.try_emplace(face.instance->id);
        if(created) {
            const auto& bones=*pose->second;
            const auto& instance=*face.instance;
            cache->second.reserve(face.mesh->vertices.size());
            for(const auto& vertex:face.mesh->vertices) {
                vec3 local{},normal{};
                float weight_sum=0.f;
                for(std::size_t k=0;k<4;++k) {
                    const float weight=vertex.weights[k];
                    if(weight<=0.f)continue;
                    if(vertex.joints[k]>=bones.size())
                        throw std::runtime_error("optics: skinned joint out of bounds");
                    const auto& m=bones[vertex.joints[k]];
                    const vec3 p=vertex.position,n=vertex.normal;
                    local=local+vec3{
                        m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
                        m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
                        m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]
                    }*weight;
                    normal=normal+vec3{
                        m[0]*n.x+m[4]*n.y+m[8]*n.z,
                        m[1]*n.x+m[5]*n.y+m[9]*n.z,
                        m[2]*n.x+m[6]*n.y+m[10]*n.z
                    }*weight;
                    weight_sum+=weight;
                }
                if(weight_sum<=0.f) {
                    local=vertex.position;
                    normal=vertex.normal;
                }
                const auto scale=instance.pose.scale;
                const auto position=instance.pose.position+
                    rotate(instance.pose.rotation,{
                        local.x*scale.x,local.y*scale.y,local.z*scale.z
                    });
                const vec3 n=rotate(instance.pose.rotation,{
                    normal.x/scale.x,normal.y/scale.y,normal.z/scale.z
                });
                cache->second.push_back({
                    position,length_squared(n)>1e-8f?
                    normalize(n):vec3{0.f,1.f,0.f}
                });
            }
        }
        const auto& v=cache->second;
        const auto ia=face.indices[0],ib=face.indices[1],ic=face.indices[2];
        if(ia>=v.size() || ib>=v.size() || ic>=v.size())continue;
        const vec3 a=v[ia].position,u=v[ib].position-a,
                   w=v[ic].position-a;
        const vec3 geometric=cross(u,w);
        if(length_squared(geometric)<=1e-10f)continue;
        auto surface=base[face.face];
        surface.origin=a;
        surface.u=u;
        surface.v=w;
        surface.normal=normalize(geometric);
        surfaces.push_back({face.face,surface});
        optics::face_normals shaded{};
        const auto encode=[&](vec3 normal,float material) {
            if(dot(normal,surface.normal)<0.f)normal=normal*-1.f;
            return std::array<float,4>{normal.x,normal.y,normal.z,material};
        };
        shaded.a=encode(v[ia].normal,face.mesh->surface.metallic);
        shaded.b=encode(v[ib].normal,face.mesh->surface.roughness);
        shaded.c=encode(v[ic].normal,face.mesh->surface.occlusion);
        normals.push_back({face.face,shaded});
    }
    if(surfaces.empty())return;
    const auto g=photon_transport->refit_geometry(image,surfaces);
    material_compositor->set_geometry_frame(image,{
        g.faces,g.nodes,g.triangles,g.scene_info,g.photons,g.offsets,
        std::uint32_t(base.size()),g.face_ids
    });
    material_compositor->update_normals(image,normals);
    // GPU timestamps do not include the CPU vertex skin/refit work.
    static std::uint32_t refits=0;
    static double total_ms=0.0,worst_ms=0.0;
    const double ms=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-refit_start).count();
    total_ms+=ms;
    worst_ms=std::max(worst_ms,ms);
    if(++refits==120u) {
        std::clog<<"optics: skinned BVH refit CPU avg="
                 <<total_ms/120.0<<" ms max="<<worst_ms<<" ms\n";
        refits=0;
        total_ms=worst_ms=0.0;
    }
}

void update_photons(vk::CommandBuffer cmd,std::uint32_t,
                    const camera::frame&)
{
    if(!transport_pending || !photon_transport ||
       !scene_renderer::has_optics())return;
    photon_transport->record_transport(cmd);
    // The reflection shader samples the transport buffers directly,
    // without reduced-resolution/interpolated surface lighting.
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
    barrier.dstAccessMask=vk::AccessFlagBits::eShaderRead;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eFragmentShader,{},
                        {barrier},{},{});
    transport_pending=false;
}

void present(vk::CommandBuffer cmd, std::uint32_t image,
             const camera::frame &view,const gpu::timestamps* timings)
{
    // Diagnostic only: an exact same-scene A/B baseline for the expensive
    // optical draw, required on tile-based GPUs where timestamps inside a
    // render pass can measure command encoding rather than fragment work.
    static const bool without_optics=std::getenv("OPTICS_PROFILE_DISABLE")!=nullptr;
    static bool announced=false;
    if(without_optics && !announced) {
        std::clog<<"optics: benchmark baseline active (optical draw disabled)\n";
        announced=true;
    }
    pc::present(cmd, image, [view,image,timings](vk::CommandBuffer command, vk::Extent2D extent)
                {
            // A nested interval isolates BVH optics from swapchain
            // presentation and editor overlay work on the GPU.
            if(timings)timings->start(command,image,6);
            if(!without_optics && material_compositor && scene_renderer::has_optics()) {
                using namespace vk::util::math;
                const float half_tan=std::tan(view.pose.fov*.5f);
                const auto& v=view.view;
                optics::camera params{};
                params.view_projection=perspective(view.pose.fov,
                    float(extent.width)/float(extent.height),.1f,1000.f)*v;
                // 2 selects the exact rasterized planar mirror shader.
                // Other scenes retain the original multi-bounce BVH tracer.
                const bool planar=scene_renderer::planar_reflection(view).has_value();
                params.eye={view.eye.x,view.eye.y,view.eye.z,
                            planar?2.f:1.f};
                params.right={v(0,0),v(0,1),v(0,2),
                              half_tan*float(extent.width)/float(extent.height)};
                params.up={v(1,0),v(1,1),v(1,2),half_tan};
                params.forward={-v(2,0),-v(2,1),-v(2,2),
                                game::options::get()->graphics.exposure};
                const auto regions=scene_renderer::optical_rects(view);
                // The nested optics timestamp can be near-zero when the
                // mirror is outside the view. Record how much screen space
                // actually enters the BVH shader, separately from present.
                static std::uint32_t observations=0,active=0;
                static std::uint64_t submitted_pixels=0;
                if(!regions.empty())++active;
                for(const auto& region:regions)
                    submitted_pixels+=std::uint64_t(region.extent.width)*
                                      region.extent.height;
                if(++observations==120) {
                    std::clog<<"optics: active_frames="<<active
                             <<"/120 scissor_pixels_per_frame="
                             <<submitted_pixels/120<<"\n";
                    observations=active=0;
                    submitted_pixels=0;
                }
                material_compositor->record(command,image,params,regions,extent);
            }
            if(timings)timings->end(command,image,6);
            editor::draw_list commands;
            append_remote_players(commands,view,extent);
            auto ui=editor::instance().paint(int(extent.width),int(extent.height));
            commands.insert(commands.end(),std::make_move_iterator(ui.begin()),std::make_move_iterator(ui.end()));
            editor::vulkan::record(command,commands,extent); });
}

} // namespace clt::postprocess
