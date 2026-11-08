#include "postprocess.hpp"
#include "scene_renderer.hpp"
#include "clt/network/connection.hpp"
#include "clt/animation/animation.hpp"
#include "srv/game/options.hpp"
#include <pcloud/pcloud.hpp>
#include <editor/editor.hpp>
#include <editor/vulkan.hpp>
#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace clt::postprocess
{
    namespace
    {
        std::shared_ptr<const ::scene::state> registered;
        pc::camera_frame convert_camera(const camera::frame& frame)
        {
            return {frame.eye,frame.pose.yaw,frame.pose.pitch,frame.pose.fov};
        }

        // Skip expensive world conversion when appended meshes do not alter
        // the light volume or its possible shadow casters. Copied unchanged
        // from the previous engine's CPU light-relevance optimization.
        bool lighting_unchanged(const ::scene::state& previous,
                                const ::scene::state& incoming)
        {
            if(incoming.instances.size()<previous.instances.size() ||
               incoming.meshes.size()!=previous.meshes.size() ||
               incoming.lights.size()!=previous.lights.size() ||
               incoming.collision_boxes.size()!=previous.collision_boxes.size() ||
               incoming.skins.size()!=previous.skins.size())
                return false;
            for(std::size_t i=0;i<previous.lights.size();++i)
            {
                const auto& a=previous.lights[i],&b=incoming.lights[i];
                if(a.position.x!=b.position.x || a.position.y!=b.position.y ||
                   a.position.z!=b.position.z || a.radius!=b.radius ||
                   a.color.x!=b.color.x || a.color.y!=b.color.y ||
                   a.color.z!=b.color.z || a.strength!=b.strength)
                    return false;
            }
            for(std::size_t i=0;i<previous.collision_boxes.size();++i)
            {
                const auto& a=previous.collision_boxes[i],&b=incoming.collision_boxes[i];
                if(a.minimum.x!=b.minimum.x || a.minimum.y!=b.minimum.y ||
                   a.minimum.z!=b.minimum.z || a.maximum.x!=b.maximum.x ||
                   a.maximum.y!=b.maximum.y || a.maximum.z!=b.maximum.z)
                    return false;
            }
            for(std::size_t i=0;i<previous.meshes.size();++i)
                if(!::scene::same_mesh(previous.meshes[i],incoming.meshes[i]))
                    return false;
            for(std::size_t i=0;i<previous.skins.size();++i)
            {
                const auto& a=previous.skins[i],&b=incoming.skins[i];
                if(a.id!=b.id || a.skeleton!=b.skeleton ||
                   a.inverse_bind!=b.inverse_bind || a.clips.size()!=b.clips.size())
                    return false;
                for(std::size_t j=0;j<a.clips.size();++j)
                    if(a.clips[j].name!=b.clips[j].name ||
                       a.clips[j].bytes!=b.clips[j].bytes)
                        return false;
            }
            if(!std::equal(previous.instances.begin(),previous.instances.end(),
                           incoming.instances.begin(),::scene::same_instance))
                return false;
        
            using namespace vk::util::math;
            for(std::size_t i=previous.instances.size();i<incoming.instances.size();++i)
            {
                const auto& instance=incoming.instances[i];
                const auto found=std::find_if(incoming.meshes.begin(),incoming.meshes.end(),
                    [&](const ::scene::mesh& mesh){return mesh.id==instance.mesh_id;});
                if(found==incoming.meshes.end())return false;
                if(found->surface.alpha_mode==2 || found->indices.empty())continue;
                // Animated deformation can extend outside the bind-pose bounds.
                // Conservatively force a rebuild for new skinned instances.
                if(found->skin_id)return false;
                float furthest_squared=0.f;
                for(const auto& vertex:found->vertices)
                    furthest_squared=std::max(furthest_squared,
                        length_squared(vertex.position));
                const float scale=std::max({std::abs(instance.pose.scale.x),
                    std::abs(instance.pose.scale.y),std::abs(instance.pose.scale.z)});
                const auto& rotation=instance.pose.rotation;
                const float rotation_length_squared=
                    rotation.x*rotation.x+rotation.y*rotation.y+
                    rotation.z*rotation.z+rotation.w*rotation.w;
                if(!std::isfinite(scale) || !std::isfinite(furthest_squared) ||
                   !std::isfinite(rotation_length_squared) ||
                   std::abs(rotation_length_squared-1.f)>0.00001f)
                    return false;
                const float radius=std::sqrt(furthest_squared)*scale+0.01f;
                for(const auto& light:incoming.lights)
                {
                    // Indirect rays can travel bounce_reach past the lit surface.
                    const float reach=radius+light.radius+bounce_reach;
                    if(length_squared(instance.pose.position-light.position)<=reach*reach)
                        return false;
                }
            }
            return true;
        }

        std::shared_ptr<pc::world> prepare_world(const ::scene::state* source)
        {
            auto prepared=std::make_shared<pc::world>();
            if(!source)return prepared;
            using namespace vk::util::math;
            prepared->lights.reserve(source->lights.size());
            for(const auto& light:source->lights)
                prepared->lights.push_back({
                    light.position,light.radius,light.color,
                    light.enabled?light.strength:0.f
                });
            prepared->collision_boxes.reserve(source->collision_boxes.size());
            for(const auto& box:source->collision_boxes)
                prepared->collision_boxes.push_back({box.minimum,box.maximum});

            // Game-only animation and mesh flattening. pc:: never depends on
            // scene::mesh, Ozz or any particular renderer/scene ownership.
            // Shadow casters use rendered mesh triangles, not collision proxies.
            // Transform each streamed instance into the same world coordinates
            // used by mesh.vert; alpha-blended surfaces do not occlude light.
            std::unordered_map<std::uint32_t,const ::scene::mesh*> mesh_lookup;
            mesh_lookup.reserve(source->meshes.size());
            for(const auto& mesh:source->meshes)mesh_lookup.emplace(mesh.id,&mesh);
            for(const auto& instance:source->instances)
            {
                const auto found=mesh_lookup.find(instance.mesh_id);
                if(found==mesh_lookup.end())continue;
                const auto* mesh=found->second;
                if(mesh->surface.alpha_mode==2)continue;
                std::vector<animation::matrix> palette;
                if(mesh->skin_id && !animation::sample(
                       mesh->skin_id,instance.animation,0.0f,palette,
                       instance.overlay_animation,instance.overlay_weight,
                       instance.overlay_root))
                    throw std::runtime_error("Cannot sample imported shadow caster skin");

                const auto transformed=[&](const ::scene::vertex& v) {
                    vec3 local=v.position;
                    if(!palette.empty())
                    {
                        local={};
                        for(std::size_t j=0;j<4;++j)
                        {
                            if(v.weights[j]<=0.0f)continue;
                            if(v.joints[j]>=palette.size())
                                throw std::runtime_error("Shadow caster joint index invalid");
                            const auto& m=palette[v.joints[j]];
                            const vec3 p=v.position;
                            local=local+vec3{
                                m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
                                m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
                                m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]
                            }*v.weights[j];
                        }
                    }
                    const auto& scale=instance.pose.scale;
                    return instance.pose.position+rotate(instance.pose.rotation,{
                        local.x*scale.x,local.y*scale.y,local.z*scale.z
                    });
                };
                std::vector<vec3> positions;
                positions.reserve(mesh->vertices.size());
                for(const auto& vertex:mesh->vertices)
                    positions.push_back(transformed(vertex));
                for(std::size_t i=0;i+2<mesh->indices.size();i+=3)
                {
                    const vec3 a=positions.at(mesh->indices[i]);
                    const vec3 b=positions.at(mesh->indices[i+1]);
                    const vec3 c=positions.at(mesh->indices[i+2]);
                    if(length_squared(cross(b-a,c-a))<1e-12f)continue;

                    // Geometry outside every point light cannot cast its shadows.
                    const vec3 lo{std::min({a.x,b.x,c.x}),
                                  std::min({a.y,b.y,c.y}),
                                  std::min({a.z,b.z,c.z})};
                    const vec3 hi{std::max({a.x,b.x,c.x}),
                                  std::max({a.y,b.y,c.y}),
                                  std::max({a.z,b.z,c.z})};
                    bool relevant=false;
                    for(const auto& light:source->lights)
                    {
                        const vec3 near{
                            std::clamp(light.position.x,lo.x,hi.x),
                            std::clamp(light.position.y,lo.y,hi.y),
                            std::clamp(light.position.z,lo.z,hi.z)
                        };
                        if(length_squared(near-light.position)<=light.radius*light.radius)
                        {
                            relevant=true;
                            break;
                        }
                    }
                    if(!relevant)continue;
                    prepared->triangles.push_back({a,b,c});
                }
            }
            return prepared;
        }
    }

    void set_exposure(float exposure){pc::set_exposure(exposure);}
    void toggle_probes(){pc::toggle_probes();}
    void updated(std::uint32_t image){pc::updated(image);}

    void create()
    {
        std::vector<pc::gbuffer> gbuffers;
        const auto count=static_cast<std::uint32_t>(
            vk::swapchain::get()->images.size());
        gbuffers.reserve(count);
        for(std::uint32_t i=0;i<count;++i)
            gbuffers.push_back(scene_renderer::outputs(i).views);
        pc::configuration shaders{};
        const auto& configured=game::options::get()->graphics;
        shaders.lighting_shader=configured.lighting_shader;
        shaders.tonemap_shader=configured.tonemap_shader;
        shaders.fullscreen_shader=configured.fullscreen_shader;
        shaders.presentation_shader=configured.presentation_shader;
        pc::create(shaders,gbuffers);
    }

    void synchronize_scene()
    {
        const auto source=connection::snapshot();
        if(source==registered)return;
        if(source && registered && lighting_unchanged(*registered,*source))
        {
            registered=source;
            return;
        }
        pc::set_scene(prepare_world(source.get()));
        pc::synchronize_scene();
        registered=source;
    }

    void release()
    {
        pc::release();
        registered.reset();
    }

    targets images(std::uint32_t image)
    {
        const auto output=pc::images(image);
        return {output.grid,output.points,output.hdr,output.ldr};
    }
    void cloud(vk::CommandBuffer cmd,std::uint32_t image)
    {
        pc::cloud(cmd,image);
    }
    void gi(vk::CommandBuffer cmd,std::uint32_t image)
    {
        pc::gi(cmd,image);
    }
    void lighting(vk::CommandBuffer cmd,std::uint32_t image,
                  const camera::frame& frame)
    {
        pc::lighting(cmd,image,convert_camera(frame));
    }
    void tonemap(vk::CommandBuffer cmd,std::uint32_t image,
                 const camera::frame& frame)
    {
        pc::tonemap(cmd,image,convert_camera(frame));
    }
    void present(vk::CommandBuffer cmd,std::uint32_t image)
    {
        pc::present(cmd,image,[](vk::CommandBuffer command,vk::Extent2D extent)
        {
            // Original game-specific editor stays in the host's presentation.
            editor::vulkan::record(command,
                editor::instance().paint(int(extent.width),int(extent.height)),
                extent);
        });
    }
}
