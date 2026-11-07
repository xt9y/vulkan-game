#include "postprocess.hpp"
#include "srv/game/options.hpp"
#include "scene_renderer.hpp"
#include "clt/network/connection.hpp"
#include "clt/view/camera.hpp"
#include <render/renderer.hpp>
#include <render/pipeline.hpp>
#include <gpu/compute.hpp>
#include <gpu/descriptors.hpp>
#include <gpu/resource.hpp>
#include <render/postprocess.hpp>
#include <memory>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <utility>
#include <iostream>
#include <numeric>
#include <functional>
#include <limits>
#include "clt/animation/animation.hpp"

namespace clt::postprocess
{
    namespace
    {
        struct image_target {
            vk::Image image{};
            vk::ImageView view{};
        };

        struct frame {
            image_target hdr;
            image_target ldr;
            gpu::owned_buffer points;
            gpu::owned_image grid;
            vk::ImageView grid_view{};
            bool dirty=true;
            vk::DescriptorSet cloud{};
            vk::DescriptorSet gi{};
            vk::DescriptorSet lighting{};
            vk::DescriptorSet tone{};
            vk::DescriptorSet presentation{};
        };

        struct state {
            std::vector<frame> frames;
            vk::Sampler sampler{};
            gpu::owned_buffer lights;
            gpu::owned_buffer triangles;
            gpu::owned_buffer nodes;
            std::shared_ptr<const ::scene::state> snapshot;
            std::uint32_t light_count=0, point_count=0, vertex_count=0, node_count=0;
            std::unique_ptr<gpu::descriptors> cloud_sets;
            std::unique_ptr<gpu::descriptors> gi_sets;
            std::unique_ptr<gpu::descriptors> lighting_sets;
            std::unique_ptr<gpu::descriptors> tone_sets;
            std::unique_ptr<gpu::descriptors> present_sets;
            std::unique_ptr<::render::postprocess_targets> targets;
            std::unique_ptr<::render::presentation_targets> presentation;
            vk::PipelineLayout present_layout{};
            std::unique_ptr<gpu::compute> cloud_program;
            std::unique_ptr<gpu::compute> gi_program;
            std::unique_ptr<gpu::compute> lighting_program;
            std::unique_ptr<gpu::compute> tone_program;
            std::unique_ptr<::render::pipeline> present_program;
        };

        state& get()
        {
            static state data;
            return data;
        }

        std::atomic<float> exposure_value{};
        std::atomic<bool> draw_probes=false;
        constexpr std::uint32_t grid_width=32,grid_height=22,grid_depth=36;
        constexpr float grid_spacing=0.2f;
        const vk::util::math::vec3 grid_origin{-3.2f,-0.2f,-3.6f};

        struct alignas(16) point_light {
            std::array<float,4> position_range{};
            std::array<float,4> color_intensity{};
        };
        static_assert(sizeof(point_light)==32);
        struct probe_point {
            std::array<float,4> position{};
            std::array<float,4> normal{};
            std::array<float,4> radiance{};
        };
        static_assert(sizeof(probe_point)==48);
        // Preorder, stackless shadow BVH: skip index, triangle start, triangle count.
        struct alignas(16) shadow_node {
            std::array<float,4> minimum{};
            std::array<float,4> maximum{};
            std::array<std::uint32_t,4> info{};
        };
        static_assert(sizeof(shadow_node)==48);
        struct cloud_push {
            std::uint32_t points,vertices,lights,nodes;
        };
        struct grid_push {
            std::uint32_t points,vertices,nodes,padding=0;
            std::array<float,4> origin_spacing{};
        };
        static_assert(sizeof(grid_push)==32);

        struct push {
            float exposure;
            std::array<float,3> padding{};
            std::array<float,4> eye_near{};
            std::array<float,4> forward_far{};
            std::array<float,4> right_tan{};
            std::array<float,4> up_tan{};
            std::array<float,4> light_count{};
            std::array<float,4> sky{};
        };
        static_assert(sizeof(push)==112);

        std::vector<vk::DescriptorSetLayoutBinding> bindings(
            std::uint32_t sampled,std::uint32_t storage,vk::ShaderStageFlags stages,
            std::uint32_t buffers=0)
        {
            std::vector<vk::DescriptorSetLayoutBinding> result(sampled+storage+buffers);
            for(std::uint32_t i=0;i<result.size();++i) {
                auto& binding=result[i];
                binding.binding=i;
                binding.descriptorCount=1;
                binding.stageFlags=stages;
                binding.descriptorType=i<sampled
                    ? vk::DescriptorType::eCombinedImageSampler
                    : i<sampled+storage ? vk::DescriptorType::eStorageImage
                                        : vk::DescriptorType::eStorageBuffer;
            }
            return result;
        }

        vk::PipelineLayout pipeline_layout(vk::DescriptorSetLayout set)
        {
            vk::PipelineLayoutCreateInfo info{};
            info.setLayoutCount=1;
            info.pSetLayouts=&set;
            return vk::device::get()->handle.createPipelineLayout(info);
        }

        void compute(vk::CommandBuffer command,const gpu::compute& program,
                     vk::DescriptorSet set,const camera::frame& frame)
        {
            const auto extent=vk::swapchain::get()->extent;
            const auto& view=frame.pose;
            const float sy=std::sin(view.yaw),cy=std::cos(view.yaw);
            const float sp=std::sin(view.pitch),cp=std::cos(view.pitch);
            const float tan_half=std::tan(view.fov*0.5f);
            const float aspect=float(extent.width)/float(extent.height);
            const auto& position=frame.eye;
            const push controls{
                exposure_value.load(std::memory_order_relaxed),{},
                {position.x,position.y,position.z,0.1f},
                {sy*cp,sp,-cy*cp,1000.0f},
                {cy,0,sy,tan_half*aspect},
                {-sy*sp,cp,cy*sp,tan_half},
                {float(get().light_count),draw_probes.load(std::memory_order_relaxed)?1.0f:0.0f,
                 float(get().vertex_count),float(get().node_count)},
                {grid_origin.x,grid_origin.y,grid_origin.z,grid_spacing}
            };
            const auto width=extent.width;
            const auto height=extent.height;
            program.dispatch(command,(width+7)/8,(height+7)/8,1,
                             {set},&controls,sizeof(controls));
        }
    }

    void toggle_probes()
    {
        draw_probes.store(!draw_probes.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }

    void updated(std::uint32_t image)
    {
        get().frames.at(image).dirty=false;
    }

    void set_exposure(float exposure)
    {
        exposure_value.store(std::max(0.0f, exposure), std::memory_order_relaxed);
    }

    void create()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        try
        {
            vk::SamplerCreateInfo sampler{};
            sampler.magFilter = vk::Filter::eNearest;
            sampler.minFilter = vk::Filter::eNearest;
            sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
            sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            data.sampler = device.createSampler(sampler);

            const auto count=static_cast<std::uint32_t>(vk::swapchain::get()->images.size());
            data.targets=std::make_unique<::render::postprocess_targets>();
            data.targets->create(count,vk::swapchain::get()->extent);
            data.presentation=std::make_unique<::render::presentation_targets>();
            data.presentation->create(vk::swapchain::get()->images,
                vk::swapchain::get()->format,vk::swapchain::get()->extent);
            data.cloud_sets=std::make_unique<gpu::descriptors>();
            data.cloud_sets->create(bindings(0,0,vk::ShaderStageFlagBits::eCompute,4),count);
            data.gi_sets=std::make_unique<gpu::descriptors>();
            data.gi_sets->create(bindings(0,1,vk::ShaderStageFlagBits::eCompute,3),count);
            data.lighting_sets=std::make_unique<gpu::descriptors>();
            data.lighting_sets->create(bindings(5,1,vk::ShaderStageFlagBits::eCompute,3),count);
            data.tone_sets=std::make_unique<gpu::descriptors>();
            data.tone_sets->create(bindings(1,1,vk::ShaderStageFlagBits::eCompute),count);
            data.present_sets=std::make_unique<gpu::descriptors>();
            data.present_sets->create(bindings(1,0,vk::ShaderStageFlagBits::eFragment),count);
            data.present_layout=pipeline_layout(data.present_sets->layout());
            data.cloud_program=std::make_unique<gpu::compute>();
            data.cloud_program->create("build/shaders/cloud.comp.spv",
                                       {data.cloud_sets->layout()},sizeof(cloud_push));
            data.gi_program=std::make_unique<gpu::compute>();
            data.gi_program->create("build/shaders/gi.comp.spv",
                                    {data.gi_sets->layout()},sizeof(grid_push));
            data.lighting_program = std::make_unique<gpu::compute>();
            data.lighting_program->create(
                game::options::get()->graphics.lighting_shader,
                {data.lighting_sets->layout()}, sizeof(push));
            data.tone_program = std::make_unique<gpu::compute>();
            data.tone_program->create(
                game::options::get()->graphics.tonemap_shader,
                {data.tone_sets->layout()}, sizeof(push));

            data.present_program = std::make_unique<::render::pipeline>();
            ::render::raster_state raster{};
            raster.pass = data.presentation->pass();
            raster.layout = data.present_layout;
            raster.cull = vk::CullModeFlagBits::eNone;
            raster.depth_test = false;
            raster.depth_write = false;
            data.present_program->create(raster, {
                {vk::ShaderStageFlagBits::eVertex,
                 game::options::get()->graphics.fullscreen_shader},
                {vk::ShaderStageFlagBits::eFragment,
                 game::options::get()->graphics.presentation_shader}
            });

            const auto storage=vk::BufferUsageFlagBits::eStorageBuffer;
            const auto memory=vk::MemoryPropertyFlagBits::eHostVisible |
                              vk::MemoryPropertyFlagBits::eHostCoherent;
            data.lights=gpu::create_buffer(64*sizeof(point_light),storage,memory);
            data.triangles=gpu::create_buffer(3*sizeof(std::array<float,4>),storage,memory);
            data.nodes=gpu::create_buffer(sizeof(shadow_node),storage,memory);

            data.frames.reserve(count);
            for(std::uint32_t i=0;i<count;++i)
            {
                data.frames.emplace_back();
                auto& current=data.frames.back();
                const auto attachments=data.targets->at(i);
                current.hdr={attachments.hdr,attachments.hdr_view};
                current.ldr={attachments.ldr,attachments.ldr_view};
                current.points=gpu::create_buffer(sizeof(probe_point),storage,memory);

                vk::ImageCreateInfo grid_info{};
                grid_info.imageType=vk::ImageType::e3D;
                grid_info.format=vk::Format::eR16G16B16A16Sfloat;
                grid_info.extent=vk::Extent3D{grid_width,grid_height,grid_depth};
                grid_info.mipLevels=1;
                grid_info.arrayLayers=1;
                grid_info.samples=vk::SampleCountFlagBits::e1;
                grid_info.tiling=vk::ImageTiling::eOptimal;
                grid_info.usage=vk::ImageUsageFlagBits::eStorage |
                                vk::ImageUsageFlagBits::eSampled;
                current.grid=gpu::create_image(grid_info,vk::MemoryPropertyFlagBits::eDeviceLocal);
                vk::ImageViewCreateInfo grid_view{};
                grid_view.image=current.grid.handle;
                grid_view.viewType=vk::ImageViewType::e3D;
                grid_view.format=grid_info.format;
                grid_view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
                current.grid_view=vk::image_view::create(grid_view);

                current.cloud=data.cloud_sets->set(i);
                current.gi=data.gi_sets->set(i);
                current.lighting=data.lighting_sets->set(i);
                current.tone=data.tone_sets->set(i);
                current.presentation=data.present_sets->set(i);

                const auto g = scene_renderer::outputs(i);
                vk::DescriptorImageInfo grid_storage{};
                grid_storage.imageView=current.grid_view;
                grid_storage.imageLayout=vk::ImageLayout::eGeneral;
                data.gi_sets->write_image(i,0,grid_storage);
                data.cloud_sets->write_buffer(i,0,vk::buffer::descriptor(current.points));
                data.cloud_sets->write_buffer(i,1,vk::buffer::descriptor(data.triangles));
                data.cloud_sets->write_buffer(i,2,vk::buffer::descriptor(data.lights));
                data.cloud_sets->write_buffer(i,3,vk::buffer::descriptor(data.nodes));
                data.gi_sets->write_buffer(i,1,vk::buffer::descriptor(current.points));
                data.gi_sets->write_buffer(i,2,vk::buffer::descriptor(data.triangles));
                data.gi_sets->write_buffer(i,3,vk::buffer::descriptor(data.nodes));

                std::array<vk::DescriptorImageInfo,6> lighting{};
                for(std::size_t j=0;j<g.views.size();++j)
                {
                    lighting[j].sampler=data.sampler;
                    lighting[j].imageView=g.views[j];
                    lighting[j].imageLayout=j==3
                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                        : vk::ImageLayout::eShaderReadOnlyOptimal;
                }
                lighting[4].sampler=data.sampler;
                lighting[4].imageView=current.grid_view;
                lighting[4].imageLayout=vk::ImageLayout::eGeneral;
                lighting[5].imageView=current.hdr.view;
                lighting[5].imageLayout=vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<lighting.size();++j)
                    data.lighting_sets->write_image(i,j,lighting[j]);
                data.lighting_sets->write_buffer(i,6,vk::buffer::descriptor(data.lights));
                data.lighting_sets->write_buffer(i,7,vk::buffer::descriptor(data.triangles));
                data.lighting_sets->write_buffer(i,8,vk::buffer::descriptor(data.nodes));
                std::array<vk::DescriptorImageInfo,2> tone{};
                tone[0].sampler = data.sampler;
                tone[0].imageView = current.hdr.view;
                tone[0].imageLayout = vk::ImageLayout::eGeneral;
                tone[1].imageView = current.ldr.view;
                tone[1].imageLayout = vk::ImageLayout::eGeneral;
                for(std::uint32_t j=0;j<tone.size();++j)
                    data.tone_sets->write_image(i,j,tone[j]);
                std::array<vk::DescriptorImageInfo,1> presented{};
                presented[0].sampler = data.sampler;
                presented[0].imageView = current.ldr.view;
                presented[0].imageLayout = vk::ImageLayout::eGeneral;
                data.present_sets->write_image(i,0,presented[0]);
            }
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    // Rogue derives its point cloud from static floor/wall geometry.
    // Reuse the server's world-space collision surfaces without expanding
    // arbitrary imported mesh polygons into brute-force visibility rays.
    void synchronize_scene()
    {
        auto& data=get();
        const auto source=connection::snapshot();
        if(source==data.snapshot)return;

        using namespace vk::util::math;
        std::vector<point_light> lights;
        std::vector<probe_point> points;
        std::vector<std::array<float,4>> triangles;
        if(source)
        {
            if(source->lights.size()>64)
                throw std::runtime_error("Scene exceeds 64 point lights");
            for(const auto& light:source->lights)
                lights.push_back({
                    {light.position.x,light.position.y,light.position.z,light.radius},
                    {light.color.x,light.color.y,light.color.z,light.strength}
                });

            // Rogue's probe cloud is built from static room surfaces, not
            // every polygon of imported character/prop meshes. The server
            // already streams those static surfaces as collision boxes.
            const vec3 limit{
                grid_origin.x+grid_width*grid_spacing,
                grid_origin.y+grid_height*grid_spacing,
                grid_origin.z+grid_depth*grid_spacing
            };
            const auto face=[&](vec3 base,vec3 u,vec3 v,vec3 normal)
            {
                // Samples use the original 0.3-unit surface spacing.
                // Restrict iteration to the probe volume, even for a huge floor.
                const float lu=length(u),lv=length(v);
                if(lu<=0.0f || lv<=0.0f)return;
                const vec3 U=u/lu,V=v/lv;
                const int nu=std::max(1,int(std::ceil(lu/0.3f)));
                const int nv=std::max(1,int(std::ceil(lv/0.3f)));
                const float du=lu/float(nu),dv=lv/float(nv);
                const auto interval=[&](vec3 axis,float step,int count)
                {
                    float lo=dot(grid_origin-base,axis);
                    float hi=dot(limit-base,axis);
                    if(lo>hi)std::swap(lo,hi);
                    const int begin=std::clamp(int(std::ceil(lo/step-0.5f)),0,count);
                    const int finish=std::clamp(int(std::ceil(hi/step+0.5f)),0,count);
                    return std::pair{begin,finish};
                };
                const auto [u0,u1]=interval(U,du,nu);
                const auto [v0,v1]=interval(V,dv,nv);
                for(int i=u0;i<u1;++i)
                for(int j=v0;j<v1;++j)
                {
                    const vec3 p=base+U*((float(i)+0.5f)*du)
                                     +V*((float(j)+0.5f)*dv);
                    if(p.x<grid_origin.x || p.y<grid_origin.y ||
                       p.z<grid_origin.z || p.x>=limit.x ||
                       p.y>=limit.y || p.z>=limit.z)continue;
                    points.push_back({
                        {p.x,p.y,p.z,0.0f},
                        {normal.x,normal.y,normal.z,0.0f},{}
                    });
                }
            };

            for(const auto& box:source->collision_boxes)
            {
                // Keep all casters that can intersect a light's radius,
                // including those just outside the probe volume.
                const bool inside=
                    box.maximum.x>=grid_origin.x-2.6f &&
                    box.maximum.y>=grid_origin.y-2.6f &&
                    box.maximum.z>=grid_origin.z-2.6f &&
                    box.minimum.x<=limit.x+2.6f &&
                    box.minimum.y<=limit.y+2.6f &&
                    box.minimum.z<=limit.z+2.6f;
                bool near_light=false;
                for(const auto& light:source->lights)
                {
                    const vec3 c{
                        std::clamp(light.position.x,box.minimum.x,box.maximum.x),
                        std::clamp(light.position.y,box.minimum.y,box.maximum.y),
                        std::clamp(light.position.z,box.minimum.z,box.maximum.z)
                    };
                    if(length_squared(c-light.position)<=light.radius*light.radius)
                    {
                        near_light=true;
                        break;
                    }
                }
                if(!inside && !near_light)continue;
                const auto a=box.minimum,b=box.maximum;
                const float dx=b.x-a.x,dy=b.y-a.y,dz=b.z-a.z;
                face({b.x,a.y,a.z},{0,dy,0},{0,0,dz},{1,0,0});
                face({a.x,a.y,b.z},{0,dy,0},{0,0,-dz},{-1,0,0});
                face({a.x,b.y,b.z},{dx,0,0},{0,0,-dz},{0,1,0});
                face({a.x,a.y,a.z},{dx,0,0},{0,0,dz},{0,-1,0});
                face({a.x,a.y,b.z},{dx,0,0},{0,dy,0},{0,0,1});
                face({b.x,a.y,a.z},{-dx,0,0},{0,dy,0},{0,0,-1});
            }
            // Shadow casters use rendered mesh triangles, not collision proxies.
            // Transform each streamed instance into the same world coordinates
            // used by mesh.vert; alpha-blended surfaces do not occlude light.
            for(const auto& instance:source->instances)
            {
                const auto mesh=std::find_if(source->meshes.begin(),source->meshes.end(),
                    [&](const ::scene::mesh& m){return m.id==instance.mesh_id;});
                if(mesh==source->meshes.end() || mesh->surface.alpha_mode==2)continue;
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
                    for(const auto& p:{a,b,c})
                        triangles.push_back({p.x,p.y,p.z,0.0f});
                }
            }
        }
        // Build a compact, preorder skip-index BVH over the actual world
        // triangles. Traversal needs no recursion or GPU stack and avoids
        // scanning high-polygon imported models for every lighting ray.
        struct primitive {
            std::array<vec3,3> positions;
            vec3 lo{},hi{},center{};
        };
        std::vector<primitive> primitives;
        primitives.reserve(triangles.size()/3);
        for(std::size_t i=0;i+2<triangles.size();i+=3)
        {
            const vec3 a{triangles[i][0],triangles[i][1],triangles[i][2]};
            const vec3 b{triangles[i+1][0],triangles[i+1][1],triangles[i+1][2]};
            const vec3 c{triangles[i+2][0],triangles[i+2][1],triangles[i+2][2]};
            const vec3 lo{std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),
                          std::min({a.z,b.z,c.z})};
            const vec3 hi{std::max({a.x,b.x,c.x}),std::max({a.y,b.y,c.y}),
                          std::max({a.z,b.z,c.z})};
            primitives.push_back({{a,b,c},lo,hi,(lo+hi)*0.5f});
        }
        std::vector<std::uint32_t> order(primitives.size());
        std::iota(order.begin(),order.end(),0u);
        std::vector<shadow_node> nodes;
        std::vector<std::array<float,4>> ordered;
        ordered.reserve(triangles.size());
        const auto axis=[](vec3 value,int i)->float {
            return i==0?value.x:(i==1?value.y:value.z);
        };
        std::function<void(std::size_t,std::size_t)> build=
            [&](std::size_t first,std::size_t end)
        {
            const auto infinity=std::numeric_limits<float>::infinity();
            vec3 lo{infinity,infinity,infinity},hi{-infinity,-infinity,-infinity};
            for(std::size_t i=first;i<end;++i)
            {
                const auto& p=primitives[order[i]];
                lo={std::min(lo.x,p.lo.x),std::min(lo.y,p.lo.y),std::min(lo.z,p.lo.z)};
                hi={std::max(hi.x,p.hi.x),std::max(hi.y,p.hi.y),std::max(hi.z,p.hi.z)};
            }
            shadow_node node{};
            node.minimum={lo.x-0.0001f,lo.y-0.0001f,lo.z-0.0001f,0.0f};
            node.maximum={hi.x+0.0001f,hi.y+0.0001f,hi.z+0.0001f,0.0f};
            const auto node_index=nodes.size();
            nodes.push_back(node);
            if(end-first<=8)
            {
                nodes[node_index].info[1]=static_cast<std::uint32_t>(ordered.size()/3);
                nodes[node_index].info[2]=static_cast<std::uint32_t>(end-first);
                for(std::size_t i=first;i<end;++i)
                    for(const auto& p:primitives[order[i]].positions)
                        ordered.push_back({p.x,p.y,p.z,0.0f});
            }
            else
            {
                const vec3 span=hi-lo;
                const int split=span.x>=span.y && span.x>=span.z ? 0
                    : span.y>=span.z ? 1 : 2;
                const auto middle=first+(end-first)/2;
                std::nth_element(order.begin()+first,order.begin()+middle,
                    order.begin()+end,[&](std::uint32_t a,std::uint32_t b)
                    {
                        return axis(primitives[a].center,split)<
                               axis(primitives[b].center,split);
                    });
                build(first,middle);
                build(middle,end);
            }
            nodes[node_index].info[0]=static_cast<std::uint32_t>(nodes.size());
        };
        if(!order.empty())build(0,order.size());
        triangles=std::move(ordered);

        std::clog << "clt: Rogue lighting geometry: "
                  << points.size() << " probes, "
                  << primitives.size() << " mesh triangles, "
                  << nodes.size() << " shadow nodes\n";

        const auto storage=vk::BufferUsageFlagBits::eStorageBuffer;
        const auto memory=vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent;
        auto new_triangles=gpu::create_buffer(
            std::max<std::size_t>(3*sizeof(std::array<float,4>),
                                  triangles.size()*sizeof(std::array<float,4>)),
            storage,memory);
        if(!triangles.empty())
            vk::buffer::write(new_triangles,triangles.data(),
                              triangles.size()*sizeof(std::array<float,4>));
        auto new_nodes=gpu::create_buffer(
            std::max<std::size_t>(sizeof(shadow_node),
                                  nodes.size()*sizeof(shadow_node)),
            storage,memory);
        if(!nodes.empty())
            vk::buffer::write(new_nodes,nodes.data(),nodes.size()*sizeof(shadow_node));
        if(!lights.empty())
            vk::buffer::write(data.lights,lights.data(),
                              lights.size()*sizeof(point_light));

        data.triangles=std::move(new_triangles);
        data.nodes=std::move(new_nodes);
        data.node_count=static_cast<std::uint32_t>(nodes.size());
        data.point_count=static_cast<std::uint32_t>(points.size());
        data.vertex_count=static_cast<std::uint32_t>(triangles.size());
        data.light_count=static_cast<std::uint32_t>(lights.size());
        for(std::uint32_t i=0;i<data.frames.size();++i)
        {
            auto& frame=data.frames[i];
            frame.points=gpu::create_buffer(
                std::max<std::size_t>(sizeof(probe_point),points.size()*sizeof(probe_point)),
                storage,memory);
            if(!points.empty())
                vk::buffer::write(frame.points,points.data(),points.size()*sizeof(probe_point));
            data.cloud_sets->write_buffer(i,0,vk::buffer::descriptor(frame.points));
            data.cloud_sets->write_buffer(i,1,vk::buffer::descriptor(data.triangles));
            data.gi_sets->write_buffer(i,1,vk::buffer::descriptor(frame.points));
            data.gi_sets->write_buffer(i,2,vk::buffer::descriptor(data.triangles));
            data.lighting_sets->write_buffer(i,7,vk::buffer::descriptor(data.triangles));
            data.cloud_sets->write_buffer(i,3,vk::buffer::descriptor(data.nodes));
            data.gi_sets->write_buffer(i,3,vk::buffer::descriptor(data.nodes));
            data.lighting_sets->write_buffer(i,8,vk::buffer::descriptor(data.nodes));
        }
        data.snapshot=source;
        for(auto& frame:data.frames)frame.dirty=true;
    }

    void release()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        for(auto& frame:data.frames)
        {
            if(frame.grid_view)vk::image_view::destroy(frame.grid_view);
            frame.grid_view=vk::ImageView{};
        }
        data.frames.clear();
        data.cloud_program.reset();
        data.gi_program.reset();
        data.lighting_program.reset();
        data.tone_program.reset();
        data.present_program.reset();
        data.presentation.reset();
        if (data.present_layout) device.destroyPipelineLayout(data.present_layout);
        data.cloud_sets.reset();
        data.gi_sets.reset();
        data.lighting_sets.reset();
        data.tone_sets.reset();
        data.present_sets.reset();
        data.targets.reset();
        data.lights.reset();
        data.triangles.reset();
        data.nodes.reset();
        if (data.sampler) device.destroySampler(data.sampler);

        data = {};
    }

    targets images(std::uint32_t image)
    {
        const auto& frame = get().frames.at(image);
        return {frame.grid.handle,frame.points.handle,frame.hdr.image,frame.ldr.image};
    }

    void cloud(vk::CommandBuffer command,std::uint32_t image)
    {
        if(!get().frames.at(image).dirty || !get().point_count)return;
        const cloud_push push{get().point_count,get().vertex_count,get().light_count,get().node_count};
        get().cloud_program->dispatch(command,(get().point_count+63)/64,1,1,
                                      {get().frames.at(image).cloud},&push,sizeof(push));
    }

    void gi(vk::CommandBuffer command,std::uint32_t image)
    {
        if(!get().frames.at(image).dirty)return;
        const grid_push push{
            get().point_count,get().vertex_count,get().node_count,0,
            {grid_origin.x,grid_origin.y,grid_origin.z,grid_spacing}
        };
        get().gi_program->dispatch(command,
            (grid_width+3)/4,(grid_height+3)/4,(grid_depth+3)/4,
            {get().frames.at(image).gi},&push,sizeof(push));
    }

    void lighting(vk::CommandBuffer command, std::uint32_t image,
                  const camera::frame& frame)
    {
        const auto& current = get().frames.at(image);
        compute(command,*get().lighting_program,current.lighting,frame);
    }

    void tonemap(vk::CommandBuffer command, std::uint32_t image,
                 const camera::frame& frame)
    {
        const auto& current = get().frames.at(image);
        compute(command,*get().tone_program,current.tone,frame);
    }

    void present(vk::CommandBuffer command, std::uint32_t image)
    {
        const auto extent = vk::swapchain::get()->extent;
        vk::ClearValue clear{};
        clear.color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 1});
        vk::RenderPassBeginInfo info{};
        info.renderPass = get().presentation->pass();
        info.framebuffer = get().presentation->framebuffer(image);
        info.renderArea = vk::Rect2D{{0, 0}, extent};
        info.clearValueCount = 1;
        info.pClearValues = &clear;

        ::render::renderer graphics(command);
        graphics.begin_pass(info);
        graphics.viewport(
            vk::Viewport{0, 0, float(extent.width), float(extent.height), 0, 1},
            vk::Rect2D{{0, 0}, extent});
        ::render::mesh triangle{};
        triangle.vertex_count = 3;
        ::render::material surface{};
        surface.pipeline = get().present_program->handle();
        surface.layout = get().present_layout;
        surface.sets = {get().frames.at(image).presentation};
        graphics.draw(triangle, surface);
        graphics.end_pass();
    }
}
