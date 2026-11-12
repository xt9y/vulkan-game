#pragma once
#include "../../server/game/options.hpp"
#include "camera.hpp"
#include "connection.hpp"
#include "texture.hpp"
#include "weapon.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <gpu/culler.hpp>
#include <gpu/descriptors.hpp>
#include <gpu/resource.hpp>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <pcloud/pcloud.hpp>
#include <render/attachments.hpp>
#include <render/geometry.hpp>
#include <render/pipeline.hpp>
#include <render/renderer.hpp>
#include <render/target.hpp>
#include <vk/vk.hpp>

namespace clt::scene_renderer
{
struct planar_view {
    camera::frame reflected;
    vk::util::math::vec4 clip;
    vk::util::math::vec4 center,axis_y,axis_z;
};

namespace
{
using namespace vk::util::math;

using instance_data = ::clt::weapon::instance_data;

struct draw_parameters
{
    mat4 view_projection;

    std::array<float, 4> base_color;
    std::array<float, 4> surface;
    std::array<float, 4> emissive;
    std::array<float, 4> clip_plane{0,0,0,1};
};

static_assert(sizeof(draw_parameters)==128);
struct gpu_mesh
{
    std::uint32_t id = 0;
    ::scene::material surface;
    std::uint32_t skin_id = 0;
    std::uint32_t joint_count = 0;
    ::render::geometry geometry;
    gpu::owned_buffer instance_buffer;
    gpu::owned_buffer bounds_buffer;
    struct per_image
    {
        gpu::owned_buffer visible;
        gpu::owned_buffer indirect;
        gpu::owned_buffer skin_palette;
        gpu::owned_buffer local_instances;
        vk::DescriptorSet set{};
    };

    std::vector<per_image> frames;
    std::vector<std::array<float, 4>> bounds;
    std::vector<instance_data> transforms;
    std::vector<::scene::instance> instances;

    vec3 center;
    float radius = 0;
    vec3 local_min{},local_max{};

    std::uint32_t index_count = 0;
};

static_assert(sizeof(instance_data) == 144);
static_assert(sizeof(::scene::vertex) == 64);
struct state
{
    std::shared_ptr<const ::scene::state> snapshot;
    std::vector<gpu_mesh> meshes;
    std::vector<gpu_mesh> local_meshes;
    std::unique_ptr<gpu::descriptors> local_sets;
    vk::DescriptorSetLayout instance_layout{};
    std::unique_ptr<gpu::descriptors> instance_sets;
    gpu::culler cull;
    struct skinned_instance {
        std::uint32_t id=0;
        std::vector<animation::matrix> palette;
    };
    std::vector<skinned_instance> skinned;
    std::uint32_t last_animation_tick = UINT32_MAX;
    std::chrono::steady_clock::time_point animation_received{};
    vk::RenderPass render_pass{};
    vk::PipelineLayout layout{};
    ::render::pipeline pipeline;
    ::render::pipeline optical_pipeline;
    struct target
    {
        ::render::attachments images;
        std::unique_ptr<::render::target> framebuffer;
    };

    std::vector<target> targets;
    std::vector<target> optical_targets;
    std::vector<target> reflection_targets;
    std::array<std::string, 2> shaders;
};

state &get()
{
    static state data;

    return data;
}

const std::vector<state::skinned_instance>& skinning_poses()
{
    return get().skinned;
}

std::array<vk::DescriptorSetLayoutBinding, 10> instance_bindings()
{
    std::array<vk::DescriptorSetLayoutBinding, 10> bindings{};

    for (std::uint32_t i = 0; i < bindings.size(); ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = (i < 4 || i == 9) ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eCombinedImageSampler;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = i < 4 ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eCompute) : i == 9 ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eVertex)
                                                                                                          : vk::ShaderStageFlags(vk::ShaderStageFlagBits::eFragment);

        if (i == 0)
            bindings[i].stageFlags |= vk::ShaderStageFlagBits::eVertex;
    }

    return bindings;
}

void draw_mesh(::render::renderer &graphics, const gpu_mesh &mesh, const gpu_mesh::per_image &frame, const draw_parameters &params, bool indirect, std::uint32_t instances, bool optical=false)
{
    ::render::mesh geometry = mesh.geometry.view();
    geometry.streams.push_back({1, frame.visible.handle, 0});
    ::render::material surface{};
    surface.pipeline = optical ? get().optical_pipeline.handle() : get().pipeline.handle();
    surface.layout = get().layout;
    surface.sets = {frame.set};
    surface.push_data = &params;
    surface.push_size = sizeof(params);
    surface.push_stages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

    if (indirect)
        graphics.draw_indirect(geometry, surface, frame.indirect.handle, 0);
    else
        graphics.draw(geometry, surface, instances);
}

draw_parameters material_parameters(const ::scene::material &m, const mat4 &view_projection, std::uint32_t joints)
{
    const auto flags = (m.base_color_texture ? 1u : 0u) | (m.metallic_roughness_texture ? 2u : 0u) | (m.normal_texture ? 4u : 0u) | (m.occlusion_texture ? 8u : 0u) | (m.emissive_texture ? 16u : 0u) | (m.alpha_mode << 5) |
                       (m.procedural_pattern == 1u ? 128u : 0u);

    return {
        view_projection, m.base_color, {m.metallic, m.roughness, m.occlusion, m.alpha_cutoff},
        {m.emissive[0], m.emissive[1], m.emissive[2],
         float(flags+(joints<<8))}, {0,0,0,1}
    };
}

struct plane
{
    vec3 normal;
    float offset;
    float magnitude;
};

std::array<plane, 6> frustum(const mat4 &m)
{
    const auto make = [&](int row, float sign)
    {
        plane p{
            {m(3, 0) + sign * m(row, 0), m(3, 1) + sign * m(row, 1), m(3, 2) + sign * m(row, 2)}, m(3, 3) + sign * m(row, 3), 0
        };

        p.magnitude = length(p.normal);

        return p;
    };

    plane near{
        {m(2, 0), m(2, 1), m(2, 2)}, m(2, 3), 0
    };

    near.magnitude = length(near.normal);

    return {make(0, 1), make(0, -1), make(1, 1), make(1, -1), near, make(2, -1)};
}

void create_pipeline()
{
    auto &data = get();
    vk::PushConstantRange range{};
    range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    range.size = sizeof(draw_parameters);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &data.instance_layout;
    data.layout = vk::pipeline_layout::create(layout_info);
    ::render::raster_state raster{};
    raster.pass = data.render_pass;
    raster.layout = data.layout;
    raster.cull = vk::CullModeFlagBits::eNone;
    raster.front = vk::FrontFace::eCounterClockwise;
    raster.depth_compare = vk::CompareOp::eLess;
    raster.bindings = {
        {0, sizeof(::scene::vertex), vk::VertexInputRate::eVertex},
        {1, sizeof(std::uint32_t), vk::VertexInputRate::eInstance}
    };

    raster.attributes = {
        {0, 0, vk::Format::eR32G32B32Sfloat, offsetof(::scene::vertex, position)},
        {1, 0, vk::Format::eR32G32B32Sfloat, offsetof(::scene::vertex, normal)},
        {2, 0, vk::Format::eR32G32Sfloat, offsetof(::scene::vertex, u)},
        {3, 1, vk::Format::eR32Uint, 0},
        {4, 0, vk::Format::eR32G32B32A32Uint, offsetof(::scene::vertex, joints)},
        {5, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(::scene::vertex, weights)}
    };

    raster.blend_attachments.resize(3);

    for (auto &blend : raster.blend_attachments)
        blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    data.pipeline.create(raster, {{vk::ShaderStageFlagBits::eVertex, data.shaders[0]},
                                  {vk::ShaderStageFlagBits::eFragment, data.shaders[1]}});
    data.optical_pipeline.create(raster, {
        {vk::ShaderStageFlagBits::eVertex, data.shaders[0]},
        {vk::ShaderStageFlagBits::eFragment, "build/shaders/optics.frag.spv"}
    });
}

std::array<vk::Format, 3> color_formats()
{
    constexpr auto format = vk::Format::eR8G8B8A8Unorm;

    return {format, format, format};
}

vk::Format depth_format()
{
    constexpr auto format = vk::Format::eD32Sfloat;

    const auto properties = vk::physical_device::get()->handle.getFormatProperties(format);
    const auto required = vk::FormatFeatureFlagBits::eDepthStencilAttachment | vk::FormatFeatureFlagBits::eSampledImage;

    if ((properties.optimalTilingFeatures & required) != required)
        throw std::runtime_error("Configured depth format unsupported");

    return format;
}

void create_render_pass()
{
    auto& data=get();
    std::array<vk::AttachmentDescription,4> attachments{};
    std::array<vk::AttachmentReference,3> colors{};
    const auto formats=color_formats();
    for(std::uint32_t i=0;i<attachments.size();++i) {
        auto& desc=attachments[i];
        desc.format=i<3?formats[i]:depth_format();
        desc.samples=vk::SampleCountFlagBits::e1;
        desc.loadOp=vk::AttachmentLoadOp::eClear;
        desc.storeOp=vk::AttachmentStoreOp::eStore;
        desc.initialLayout=vk::ImageLayout::eUndefined;
        desc.finalLayout=i<3?vk::ImageLayout::eShaderReadOnlyOptimal:
                             vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        if(i<3)colors[i]={i,vk::ImageLayout::eColorAttachmentOptimal};
    }
    vk::AttachmentReference depth{3,vk::ImageLayout::eDepthStencilAttachmentOptimal};
    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint=vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount=std::uint32_t(colors.size());
    subpass.pColorAttachments=colors.data();
    subpass.pDepthStencilAttachment=&depth;
    std::array<vk::SubpassDependency,2> barriers{};
    barriers[0].srcSubpass=VK_SUBPASS_EXTERNAL;
    barriers[0].dstSubpass=0;
    barriers[0].srcStageMask=vk::PipelineStageFlagBits::eTopOfPipe;
    barriers[0].dstStageMask=vk::PipelineStageFlagBits::eColorAttachmentOutput|
                             vk::PipelineStageFlagBits::eEarlyFragmentTests;
    barriers[0].dstAccessMask=vk::AccessFlagBits::eColorAttachmentWrite|
                              vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    barriers[1].srcSubpass=0;
    barriers[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    barriers[1].srcStageMask=vk::PipelineStageFlagBits::eColorAttachmentOutput|
                             vk::PipelineStageFlagBits::eEarlyFragmentTests|
                             vk::PipelineStageFlagBits::eLateFragmentTests;
    barriers[1].dstStageMask=vk::PipelineStageFlagBits::eComputeShader|
                             vk::PipelineStageFlagBits::eFragmentShader;
    barriers[1].srcAccessMask=vk::AccessFlagBits::eColorAttachmentWrite|
                              vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    barriers[1].dstAccessMask=vk::AccessFlagBits::eShaderRead;
    vk::RenderPassCreateInfo info{};
    info.attachmentCount=std::uint32_t(attachments.size());
    info.pAttachments=attachments.data();
    info.subpassCount=1;
    info.pSubpasses=&subpass;
    info.dependencyCount=std::uint32_t(barriers.size());
    info.pDependencies=barriers.data();
    data.render_pass=vk::render_pass::create(info);
}

void create_target(bool optical=false,bool reflected=false)
{
    auto &data = get();

    const auto extent = vk::swapchain::get()->extent;
    const auto formats = color_formats();

    state::target target{};
    target.images.create(extent, std::vector<vk::Format>(formats.begin(), formats.end()), depth_format());
    target.framebuffer = std::make_unique<::render::target>();
    target.framebuffer->create_framebuffer(data.render_pass,
                                           {target.images.color(0), target.images.color(1), target.images.color(2), target.images.depth()}, extent);
    if(reflected)data.reflection_targets.push_back(std::move(target));
    else if(optical)data.optical_targets.push_back(std::move(target));
    else data.targets.push_back(std::move(target));
}
} // namespace
void init(const char *vertex_path, const char *fragment_path)
{
    if (!vertex_path || !fragment_path || !*vertex_path || !*fragment_path)
        throw std::invalid_argument("Scene shader paths must not be empty");
    auto &data = get();
    data.shaders = {vertex_path, fragment_path};

    const auto bindings = instance_bindings();
    const auto device = vk::device::get()->handle;

    vk::DescriptorSetLayoutCreateInfo info{};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    data.instance_layout = device.createDescriptorSetLayout(info);
    texture::init();
    weapon::initialize();
    texture::pin(weapon::assets());
    texture::synchronize(::scene::state{});
    data.cull.create(game::options::get()->graphics.culling_shader, data.instance_layout);
}

pc::gbuffer target_outputs(const std::vector<state::target>& targets,
                           std::uint32_t image)
{
    const auto& images=targets.at(image).images;
    return {images.color(0),images.color(1),images.color(2),images.depth()};
}
pc::gbuffer optical_outputs(std::uint32_t image) {return target_outputs(get().optical_targets,image);}
pc::gbuffer reflection_outputs(std::uint32_t image) {return target_outputs(get().reflection_targets,image);}

bool has_optics()
{
    const auto& scene=get().snapshot;
    return scene && std::any_of(scene->instances.begin(),scene->instances.end(),
        [](const auto& i){return i.pattern==2u || i.pattern==3u;});
}

// One isolated glass object can transmit the already shaded HDR world
// without BVH traversal; nested glass/mirrors continue using full optics.
bool single_glass()
{
    const auto& scene=get().snapshot;
    if(!scene)return false;
    bool found=false;
    for(const auto& instance:scene->instances) {
        if(instance.pattern!=2u && instance.pattern!=3u)continue;
        const auto& scale=instance.pose.scale;
        const bool solid=(instance.mesh_id==::scene::unit_sphere_mesh_id ||
            instance.mesh_id==::scene::unit_cube_mesh_id) &&
            std::min({std::abs(scale.x),std::abs(scale.y),
                      std::abs(scale.z)})>=.06f;
        if(instance.pattern!=2u || found || solid)return false;
        found=true;
    }
    return found;
}

// Only planar mirrors need a reflected-camera raster pass. Glass already
// traces real reflections in optics, so a second full scene render is wasted.
std::optional<planar_view> planar_reflection(const camera::frame& camera)
{
    // Compare exact rasterized planar mirrors with the original per-pixel
    // BVH path without changing the world or shader build.
    static const bool disabled=std::getenv("OPTICS_PLANAR_DISABLE")!=nullptr;
    if(disabled)return std::nullopt;
    const auto& scene=get().snapshot;
    if(!scene)return std::nullopt;
    const ::scene::instance* optical=nullptr;
    for(const auto& instance:scene->instances) {
        // A glass object does not invalidate the single planar mirror.
        if(instance.pattern!=3u)continue;
        if(optical)return std::nullopt;
        optical=&instance;
    }
    if(!optical ||
       optical->mesh_id!=::scene::unit_cube_mesh_id ||
       std::abs(optical->pose.scale.x)>.5f ||
       std::abs(optical->pose.scale.x)<.001f ||
       std::abs(optical->pose.scale.y)<1.f ||
       std::abs(optical->pose.scale.z)<1.f)
        return std::nullopt;
    const auto found=std::find_if(scene->meshes.begin(),scene->meshes.end(),
        [&](const auto& mesh){return mesh.id==optical->mesh_id;});
    if(found==scene->meshes.end() || found->skin_id || found->vertices.empty())
        return std::nullopt;
    float lo=found->vertices.front().position.x;
    float hi=lo;
    for(const auto& vertex:found->vertices) {
        lo=std::min(lo,vertex.position.x);
        hi=std::max(hi,vertex.position.x);
    }
    const vec3 axis=normalize(rotate(optical->pose.rotation,{1,0,0}));
    const float side=dot(camera.eye-optical->pose.position,axis);
    const vec3 N=side>=0.f?axis:axis*-1.f;
    const float face=side>=0.f?hi:lo;
    const vec3 point=optical->pose.position+
                     axis*(face*optical->pose.scale.x);
    float min_y=found->vertices.front().position.y;
    float max_y=min_y,min_z=found->vertices.front().position.z,max_z=min_z;
    for(const auto& vertex:found->vertices) {
        min_y=std::min(min_y,vertex.position.y);
        max_y=std::max(max_y,vertex.position.y);
        min_z=std::min(min_z,vertex.position.z);
        max_z=std::max(max_z,vertex.position.z);
    }
    const vec3 center=point+rotate(optical->pose.rotation,{
        0.f,(min_y+max_y)*.5f*optical->pose.scale.y,
        (min_z+max_z)*.5f*optical->pose.scale.z});
    const vec3 axis_y=normalize(rotate(optical->pose.rotation,{0,1,0}));
    const vec3 axis_z=normalize(rotate(optical->pose.rotation,{0,0,1}));
    const float distance=dot(camera.eye-point,N);
    if(distance<.02f)return std::nullopt;
    const auto reflect=[&](vec3 v){return v-N*(2.f*dot(v,N));};
    const vec3 virtual_eye=camera.eye-N*(2.f*distance);
    const vec3 forward{-camera.view(2,0),-camera.view(2,1),-camera.view(2,2)};
    const vec3 up{camera.view(1,0),camera.view(1,1),camera.view(1,2)};
    const vec3 reflected_forward=reflect(forward);
    const mat4 reflected_view=look_at(virtual_eye,
        virtual_eye+reflected_forward,reflect(up));
    return planar_view{
        {camera.pose,reflected_view,virtual_eye},
        {N.x,N.y,N.z,-dot(N,point)+.0002f},
        {center.x,center.y,center.z,1.f},
        {axis_y.x,axis_y.y,axis_y.z,
            (max_y-min_y)*.5f*std::abs(optical->pose.scale.y)},
        {axis_z.x,axis_z.y,axis_z.z,
            (max_z-min_z)*.5f*std::abs(optical->pose.scale.z)}
    };
}

// Conservative screen-space bounds for visible glass/mirror instances.
// Disjoint rectangles avoid overshading; glass uses a cheap compositing
// shader and only reflective mirror pixels traverse the scene BVH.
std::vector<vk::Rect2D> optical_rects(const camera::frame& camera)
{
    const auto extent=vk::swapchain::get()->extent;
    std::vector<vk::Rect2D> regions;
    if(!extent.width || !extent.height)return regions;
    const auto vp=perspective(camera.pose.fov,
        float(extent.width)/float(extent.height),.1f,1000.f)*camera.view;
    for(const auto& mesh:get().meshes) {
        if(mesh.instances.empty())continue;
        for(const auto& instance:mesh.instances) {
            if(instance.pattern!=2u && instance.pattern!=3u)continue;
            // Bind-pose bounds do not conservatively contain every skin
            // deformation. Never clip a reflective animated surface.
            if(mesh.joint_count)
                return {vk::Rect2D{{0,0},extent}};
            std::array<vec4,8> clips{};
            for(int corner=0;corner<8;++corner) {
                const auto local=vec3{
                    (corner&1)?mesh.local_max.x:mesh.local_min.x,
                    (corner&2)?mesh.local_max.y:mesh.local_min.y,
                    (corner&4)?mesh.local_max.z:mesh.local_min.z};
                const auto world=::scene::transform_position(instance.pose,local);
                clips[corner]=vp*vec4{world,1.f};
            }
            // Homogeneous frustum rejection also handles optical objects
            // wholly behind the camera. The old early near-plane fallback
            // incorrectly made those objects shade the entire screen.
            const auto outside=[&](auto predicate) {
                return std::all_of(clips.begin(),clips.end(),predicate);
            };
            if(outside([](const vec4& p){return p.z<0.f;}) ||
               outside([](const vec4& p){return p.z>p.w;}) ||
               outside([](const vec4& p){return p.x<-p.w;}) ||
               outside([](const vec4& p){return p.x>p.w;}) ||
               outside([](const vec4& p){return p.y<-p.w;}) ||
               outside([](const vec4& p){return p.y>p.w;}))continue;
            float min_x=float(extent.width),min_y=float(extent.height);
            float max_x=0.f,max_y=0.f;
            bool projected=false;
            const auto include=[&](const vec4& p) {
                // Near-plane edge intersections can round slightly below zero.
                if(p.z<-.00001f || p.w<=.00001f)return;
                const float x=(p.x/p.w*.5f+.5f)*float(extent.width);
                const float y=(p.y/p.w*.5f+.5f)*float(extent.height);
                if(!std::isfinite(x) || !std::isfinite(y))return;
                min_x=std::min(min_x,x);max_x=std::max(max_x,x);
                min_y=std::min(min_y,y);max_y=std::max(max_y,y);
                projected=true;
            };
            for(const auto& p:clips)include(p);
            // The perspective projection is not bounded by corners when
            // an edge crosses the near plane. Include all twelve clipped
            // box edges instead of conservatively drawing the whole frame.
            for(int corner=0;corner<8;++corner)
                for(int axis=0;axis<3;++axis) {
                    const int bit=1<<axis;
                    if(corner&bit)continue;
                    const auto& a=clips[corner];
                    const auto& b=clips[corner|bit];
                    if((a.z>=0.f)==(b.z>=0.f))continue;
                    const float t=-a.z/(b.z-a.z);
                    include({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,
                             a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t});
                }
            if(!projected)continue;
            // Clamp floating bounds before int conversion: very close
            // geometry can project far beyond the representable int range.
            const int x0=int(std::clamp(std::floor(min_x)-2.f,0.f,float(extent.width)));
            const int y0=int(std::clamp(std::floor(min_y)-2.f,0.f,float(extent.height)));
            const int x1=int(std::clamp(std::ceil(max_x)+2.f,0.f,float(extent.width)));
            const int y1=int(std::clamp(std::ceil(max_y)+2.f,0.f,float(extent.height)));
            if(x1<=x0 || y1<=y0)continue;
            vk::Rect2D region{{x0,y0},{std::uint32_t(x1-x0),
                                       std::uint32_t(y1-y0)}};
            // Store the exact projected bounds. The final union below avoids
            // both overlap re-shading and excess pixels from bounding-box
            // merging of L-shaped / disjoint material silhouettes.
            regions.push_back(region);
        }
    }
    // Sweep horizontal bands through all rectangle y-edges. Within each
    // band, merge overlapping x intervals; then join identical intervals
    // vertically. Output is non-overlapping and covers exactly the union
    // of the original conservative projections (no image quality change).
    if(regions.size()<2)return regions;
    // The two complexity fallbacks share the same conservative rectangle.
    // The material mask continues to reject unused pixels.
    const auto enclosing=[&]() {
        int left=int(extent.width),top=int(extent.height),right=0,bottom=0;
        for(const auto& r:regions) {
            left=std::min(left,r.offset.x);
            top=std::min(top,r.offset.y);
            right=std::max(right,r.offset.x+int(r.extent.width));
            bottom=std::max(bottom,r.offset.y+int(r.extent.height));
        }
        return vk::Rect2D{{left,top},
            {std::uint32_t(right-left),std::uint32_t(bottom-top)}};
    };
    if(regions.size()>64)return {enclosing()};
    std::vector<int> y_edges;
    y_edges.reserve(regions.size()*2);
    for(const auto& r:regions) {
        y_edges.push_back(r.offset.y);
        y_edges.push_back(r.offset.y+int(r.extent.height));
    }
    std::sort(y_edges.begin(),y_edges.end());
    y_edges.erase(std::unique(y_edges.begin(),y_edges.end()),y_edges.end());
    std::vector<vk::Rect2D> disjoint;
    // The previous band is the only one eligible for vertical merging.
    // Index intervals directly instead of scanning every emitted rectangle.
    std::map<std::pair<int,int>,std::size_t> active;
    for(std::size_t band=1;band<y_edges.size();++band) {
        const int top=y_edges[band-1],bottom=y_edges[band];
        if(top==bottom)continue;
        std::vector<std::pair<int,int>> intervals;
        for(const auto& r:regions)
            if(r.offset.y<=top && r.offset.y+int(r.extent.height)>=bottom)
                intervals.emplace_back(r.offset.x,r.offset.x+int(r.extent.width));
        std::sort(intervals.begin(),intervals.end());
        std::map<std::pair<int,int>,std::size_t> next;
        for(std::size_t i=0;i<intervals.size();) {
            const int left=intervals[i].first;
            int right=intervals[i++].second;
            while(i<intervals.size() && intervals[i].first<=right)
                right=std::max(right,intervals[i++].second);
            if(right<=left)continue;
            const auto key=std::pair{left,right};
            const auto it=active.find(key);
            if(it!=active.end()) {
                disjoint[it->second].extent.height+=std::uint32_t(bottom-top);
                next.emplace(key,it->second);
            } else {
                next.emplace(key,disjoint.size());
                disjoint.push_back({{left,top},
                    {std::uint32_t(right-left),std::uint32_t(bottom-top)}});
            }
        }
        active.swap(next);
    }
    // Bound CPU/draw overhead on scenes with many separate optical pieces.
    // This fallback still has exact output because the shader mask discards
    // all non-optical fragments inside the larger scissor.
    if(disjoint.size()>32)return {enclosing()};
    return disjoint;
}

pc::gbuffer outputs(std::uint32_t image) {return target_outputs(get().targets,image);}

void release_local_frames()
{
    auto &data = get();

    for (auto &mesh : data.local_meshes)
        mesh.frames.clear();
    data.local_sets.reset();
}

void bind_storage(vk::DescriptorSet set, const std::array<vk::Buffer, 5> &buffers)
{
    std::array<vk::DescriptorBufferInfo, 5> infos{};
    std::array<vk::WriteDescriptorSet, 5> writes{};

    for (std::uint32_t i = 0; i < buffers.size(); ++i)
    {
        infos[i].buffer = buffers[i];
        infos[i].range = VK_WHOLE_SIZE;
        writes[i].dstSet = set;
        writes[i].dstBinding = i == 4 ? 9 : i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[i].pBufferInfo = &infos[i];
    }

    vk::device::get()->handle.updateDescriptorSets(writes, {});
}

gpu_mesh upload_mesh(const ::scene::mesh &mesh, gpu::upload::batch &transfers)
{
    gpu_mesh result;
    result.id = mesh.id;
    result.skin_id = mesh.skin_id;
    result.joint_count = animation::joints(mesh.skin_id);
    result.surface = mesh.surface;
    result.index_count = std::uint32_t(mesh.indices.size());
    if(!mesh.vertices.empty()) {
        result.local_min=result.local_max=mesh.vertices.front().position;
        for(const auto& vertex:mesh.vertices) {
            const auto p=vertex.position;
            result.local_min={
                std::min(result.local_min.x,p.x),
                std::min(result.local_min.y,p.y),
                std::min(result.local_min.z,p.z)};
            result.local_max={
                std::max(result.local_max.x,p.x),
                std::max(result.local_max.y,p.y),
                std::max(result.local_max.z,p.z)};
        }
        result.center=(result.local_min+result.local_max)*.5f;
        result.radius=length((result.local_max-result.local_min)*.5f);
    }
    result.geometry.upload(transfers, mesh.vertices.data(), mesh.vertices.size() * sizeof(::scene::vertex), mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t), result.index_count);

    return result;
}

void create_local_frames()
{
    auto &data = get();
    const auto &source = weapon::assets();

    if (source.meshes.empty())
        return;

    if (data.local_meshes.empty()) {
        gpu::upload::batch transfers;

        for (const auto &mesh : source.meshes)
            data.local_meshes.push_back(upload_mesh(mesh, transfers));
        transfers.finish();
    }

    const auto count = std::uint32_t(data.local_meshes.size() * vk::swapchain::get()->images.size());
    const auto schema = instance_bindings();

    data.local_sets = std::make_unique<gpu::descriptors>();
    data.local_sets->create(std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(), schema.end()), count);
    std::uint32_t descriptor_index = 0;
    std::array<std::uint32_t, 192> identities{};

    for (std::uint32_t i = 0; i < identities.size(); ++i)
        identities[i] = i;

    for (auto &mesh : data.local_meshes)
    {
        mesh.frames.resize(vk::swapchain::get()->images.size());

        for (auto &frame : mesh.frames)
        {
            const auto memory = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
            frame.local_instances = gpu::create_buffer(192 * sizeof(instance_data), vk::BufferUsageFlagBits::eStorageBuffer, memory);
            frame.visible = gpu::create_buffer(sizeof(identities), vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer, memory);
            vk::buffer::write(frame.visible, identities.data(), sizeof(identities));
            frame.skin_palette = gpu::create_buffer(std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix), vk::BufferUsageFlagBits::eStorageBuffer, memory);
            frame.set = data.local_sets->set(descriptor_index++);
            bind_storage(frame.set, {frame.local_instances.handle, frame.local_instances.handle, frame.local_instances.handle, frame.local_instances.handle, frame.skin_palette.handle});
            texture::bind(frame.set, mesh.surface);
        }
    }
}

void release_frame_resources()
{
    auto &data = get();
    release_local_frames();

    for (auto &mesh : data.meshes)
        mesh.frames.clear();
    data.instance_sets.reset();
    data.pipeline.reset();
    data.optical_pipeline.reset();
    vk::pipeline_layout::destroy(data.layout);
    data.layout = nullptr;
    data.targets.clear();
    data.optical_targets.clear();
    data.reflection_targets.clear();
    vk::render_pass::destroy(data.render_pass);
    data.render_pass = nullptr;
}

void rebuild_instance_frames();
void create_frame_resources()
{
    try
    {
        create_render_pass();

        for (std::size_t i = 0; i < vk::swapchain::get()->images.size(); ++i)
        {
            create_target();
            create_target(true);
            create_target(false,true);
        }
        create_pipeline();
        create_local_frames();
        rebuild_instance_frames();
    }

    catch (...)
    {
        release_frame_resources();
        throw;
    }
}

void append_instance(gpu_mesh &mesh, const ::scene::instance &instance)
{
    const auto &pose = instance.pose;
    const vec3 center{
        mesh.center.x * pose.scale.x, mesh.center.y * pose.scale.y, mesh.center.z * pose.scale.z
    };

    const vec3 inverse{
        pose.scale.x ? 1.0f / pose.scale.x : 0.0f, pose.scale.y ? 1.0f / pose.scale.y : 0.0f, pose.scale.z ? 1.0f / pose.scale.z : 0.0f
    };

    const float max_scale = std::max({std::abs(pose.scale.x), std::abs(pose.scale.y), std::abs(pose.scale.z)});

    mesh.instances.push_back(instance);
    // 4 is a render-only glass mode: a closed solid with two refractive
    // interfaces. Thin panes retain kind 2 and the cheap transparent path.
    const bool glass_solid=instance.pattern==2u &&
        (instance.mesh_id==::scene::unit_sphere_mesh_id ||
         instance.mesh_id==::scene::unit_cube_mesh_id) &&
        std::min({std::abs(pose.scale.x),std::abs(pose.scale.y),
                  std::abs(pose.scale.z)})>=.06f;
    const float kind=instance.pattern==UINT32_MAX?-1.f:
        float(glass_solid?4u:instance.pattern);
    const unsigned strength=static_cast<unsigned>(std::lround(std::clamp(instance.optical_strength,0.f,1.f)*255.f));
    const unsigned mattness=static_cast<unsigned>(std::lround(std::clamp(instance.optical_mattness,0.f,1.f)*255.f));
    const float packed=(instance.pattern==2u || instance.pattern==3u) ?
        kind+float((strength<<8u)|mattness)/65536.f : kind;
    mesh.transforms.push_back({trs(pose.position, pose.rotation, pose.scale), rotation(pose.rotation) * scale(inverse),
                               {instance.tint.x, instance.tint.y, instance.tint.z, packed}});

    const auto position = pose.position + rotate(pose.rotation, center);
    mesh.bounds.push_back({position.x, position.y, position.z, mesh.radius * max_scale});
}

void bind_instances(gpu_mesh &mesh)
{
    if (!mesh.instance_buffer.handle || !mesh.bounds_buffer.handle) return;

    for (auto &frame : mesh.frames)
    {
        if (!frame.set || !frame.visible.handle || !frame.indirect.handle)
            continue;
        bind_storage(frame.set, {mesh.instance_buffer.handle, mesh.bounds_buffer.handle, frame.visible.handle, frame.indirect.handle, frame.skin_palette.handle});
        texture::bind(frame.set, mesh.surface);
    }
}

void refresh_instances(gpu_mesh &mesh, std::size_t first = 0, gpu::upload::batch *shared_upload = nullptr)
{
    if (mesh.transforms.empty()) return;
    gpu::upload::batch local;
    auto &transfers = shared_upload ? *shared_upload : local;

    const auto upload = [&](gpu::owned_buffer &buffer, const void *source, std::size_t stride, std::size_t count)
    {
        const auto bytes = count * stride;

        if (!buffer.handle || buffer.size < bytes)
            buffer = transfers.add(source, bytes, vk::BufferUsageFlagBits::eStorageBuffer, std::max<vk::DeviceSize>(bytes, buffer.size * 2));
        else if (first < count)
            transfers.write(buffer, reinterpret_cast<const std::uint8_t *>(source) + first * stride, (count - first) * stride, first * stride);
    };

    upload(mesh.instance_buffer, mesh.transforms.data(), sizeof(instance_data), mesh.transforms.size());
    upload(mesh.bounds_buffer, mesh.bounds.data(), sizeof(mesh.bounds.front()), mesh.bounds.size());
    mesh.frames.resize(vk::swapchain::get()->images.size());

    const auto palette_size = std::max<std::size_t>(sizeof(animation::matrix), mesh.instances.size() * std::max(1u, mesh.joint_count) * sizeof(animation::matrix));
    const auto visible_bytes = mesh.bounds.size() * sizeof(std::uint32_t);

    for (auto &frame : mesh.frames)
    {
        if (!frame.visible.handle || frame.visible.size < visible_bytes)
            frame.visible = gpu::create_buffer(std::max<vk::DeviceSize>(visible_bytes, frame.visible.size * 2), vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
                                               vk::MemoryPropertyFlagBits::eDeviceLocal);
        if (!frame.indirect.handle)
            frame.indirect = gpu::create_buffer(sizeof(vk::DrawIndexedIndirectCommand), vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eDeviceLocal);

        if (!frame.skin_palette.handle || frame.skin_palette.size < palette_size)
            frame.skin_palette = gpu::create_buffer(palette_size, vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    if (!shared_upload) transfers.finish();
    bind_instances(mesh);
}

void prepare_instance_frames(std::vector<gpu_mesh> &meshes, std::unique_ptr<gpu::descriptors> &sets, bool retained)
{
    if (meshes.empty()) return;

    const auto count = std::max<std::size_t>(1, meshes.size()) * std::max<std::size_t>(1, vk::swapchain::get()->images.size());
    const auto schema = instance_bindings();

    sets = std::make_unique<gpu::descriptors>();
    sets->create(std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(), schema.end()), static_cast<std::uint32_t>(count));
    std::uint32_t index = 0;
    gpu::upload::batch transfers;

    for (auto &mesh : meshes)
    {
        if (mesh.transforms.empty()) continue;
        refresh_instances(mesh, retained ? mesh.transforms.size() : 0, &transfers);

        for (auto &frame : mesh.frames)
            frame.set = sets->set(index++);
        bind_instances(mesh);
    }

    transfers.finish();
}

void rebuild_instance_frames()
{
    auto &data = get();
    prepare_instance_frames(data.meshes, data.instance_sets, true);
}

bool needs_sync()
{
    return connection::snapshot() != get().snapshot;
}

void synchronize()
{
    auto &data = get();

    const auto source = connection::snapshot();

    if (source == data.snapshot) return;

    const bool retained = data.snapshot && source && source->textures.size() >= data.snapshot->textures.size() &&
                          source->meshes.size() == data.snapshot->meshes.size();

    if (retained) {
        animation::synchronize(*source);
        if(source->atlas_revision!=data.snapshot->atlas_revision)
            texture::synchronize(*source);
        std::unordered_map<std::uint32_t, std::vector<::scene::instance>> grouped;
        for (const auto &instance : source->instances) grouped[instance.mesh_id].push_back(instance);
        gpu::upload::batch upload;
        bool activate = false;
        for (auto &mesh : data.meshes) {
            const auto &incoming = grouped[mesh.id];
            std::size_t first = 0;
            while (first < mesh.instances.size() && first < incoming.size() &&
                   ::scene::same_geometry(mesh.instances[first], incoming[first])) ++first;
            if (first == mesh.instances.size() && first == incoming.size()) continue;
            activate = activate || (mesh.instances.empty() && !incoming.empty());
            mesh.instances.resize(first);
            mesh.transforms.resize(first);
            mesh.bounds.resize(first);
            for (std::size_t i = first; i < incoming.size(); ++i) append_instance(mesh, incoming[i]);
            refresh_instances(mesh, first, &upload);
        }
        upload.finish();
        if (activate) rebuild_instance_frames();
        data.snapshot = source;
        return;
    }

    std::vector<gpu_mesh> fresh;
    std::unique_ptr<gpu::descriptors> sets;

    if (source) {
        animation::synchronize(*source);
        texture::synchronize(*source);
        fresh.reserve(source->meshes.size());
        gpu::upload::batch upload;

        for (const auto &mesh : source->meshes)
        {
            gpu_mesh gpu = upload_mesh(mesh, upload);

            fresh.push_back(std::move(gpu));
        }

        upload.finish();
        std::unordered_map<std::uint32_t, std::size_t> lookup;
        lookup.reserve(fresh.size());

        for (std::size_t i = 0; i < fresh.size(); ++i)
            lookup.emplace(fresh[i].id, i);

        for (const auto &instance : source->instances)
        {
            const auto found = lookup.find(instance.mesh_id);

            if (found != lookup.end() && fresh[found->second].index_count)
                append_instance(fresh[found->second], instance);
        }

        prepare_instance_frames(fresh, sets, false);
    }
    else {
        animation::reset();
        texture::reset();
        texture::synchronize(::scene::state{});
    }

    data.meshes.clear();
    data.instance_sets = std::move(sets);
    data.meshes = std::move(fresh);
    data.snapshot = source;

    for (auto &mesh : data.local_meshes)
        for (auto &frame : mesh.frames)
            texture::bind(frame.set, mesh.surface);
}

// Shared frustum cull for the camera and its reflected raster view.
// The reflected pass reuses the same indirect/instance buffers only after
// the original draw's vertex and indirect reads have completed.
void cull_world(vk::CommandBuffer command,std::uint32_t image,
                const mat4& vp,bool reflected=false)
{
    auto& data=get();
    if(!data.snapshot)return;
    if(reflected) {
        vk::MemoryBarrier reuse{};
        reuse.srcAccessMask=vk::AccessFlagBits::eIndirectCommandRead |
                            vk::AccessFlagBits::eVertexAttributeRead;
        reuse.dstAccessMask=vk::AccessFlagBits::eTransferWrite |
                            vk::AccessFlagBits::eShaderWrite;
        command.pipelineBarrier(
            vk::PipelineStageFlagBits::eDrawIndirect |
            vk::PipelineStageFlagBits::eVertexInput,
            vk::PipelineStageFlagBits::eTransfer |
            vk::PipelineStageFlagBits::eComputeShader,{},{reuse},{},{});
    }
    const auto frustum_planes=frustum(vp);
    std::array<vec4,6> planes{};
    for(std::size_t i=0;i<planes.size();++i) {
        const auto& p=frustum_planes[i];
        const float inv=1.f/std::max(p.magnitude,1e-6f);
        planes[i]={p.normal.x*inv,p.normal.y*inv,p.normal.z*inv,p.offset*inv};
    }
    std::vector<vk::Buffer> indirects;
    indirects.reserve(data.meshes.size());
    for(const auto& mesh:data.meshes)
        if(!mesh.transforms.empty() && !mesh.joint_count)
            indirects.push_back(mesh.frames.at(image).indirect.handle);
    data.cull.begin(command,indirects);
    for(const auto& mesh:data.meshes) {
        if(mesh.transforms.empty() || mesh.joint_count)continue;
        data.cull.dispatch(command,mesh.frames.at(image).set,
            planes,std::uint32_t(mesh.transforms.size()),mesh.index_count);
    }
    data.cull.end(command);
}


// All three G-buffers have the same clear values, viewport and render pass.
// Keep the callback inside the pass so framebuffer/clear pointers stay valid.
template<class Draw>
void draw_target(vk::CommandBuffer command, std::uint32_t image,
                 const std::vector<state::target>& targets, Draw draw)
{
    const auto extent = vk::swapchain::get()->extent;
    std::array<vk::ClearValue, 4> clear{};
    for (std::size_t i = 0; i < 3; ++i)
        clear[i].color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0});
    clear[3].depthStencil = vk::ClearDepthStencilValue(1.f, 0);
    vk::RenderPassBeginInfo begin{};
    begin.renderPass = get().render_pass;
    begin.framebuffer = targets.at(image).framebuffer->framebuffer();
    begin.renderArea = vk::Rect2D{{0, 0}, extent};
    begin.clearValueCount = clear.size();
    begin.pClearValues = clear.data();
    ::render::renderer graphics(command);
    graphics.begin_pass(begin);
    graphics.viewport(vk::Viewport{0, 0, float(extent.width), float(extent.height), .1f, 1.f}, begin.renderArea);
    draw(graphics);
    graphics.end_pass();
}

void draw_world(::render::renderer& graphics, std::uint32_t image,
                const mat4& vp, const std::array<float, 4>& clip = {0, 0, 0, 1}, bool optical = false)
{
    for (const auto& mesh : get().meshes) {
        if (mesh.transforms.empty()) continue;
        if (optical && std::none_of(mesh.instances.begin(), mesh.instances.end(),
            [](const auto& i) { return i.pattern == 2u || i.pattern == 3u; })) continue;
        auto params = material_parameters(mesh.surface, vp, mesh.joint_count);
        params.clip_plane = clip;
        // Skinned instances are already sampled and uploaded in exact
        // CPU order. Avoid GPU compacted indices for their palette lookup.
        draw_mesh(graphics,mesh,mesh.frames.at(image),params,
                  mesh.joint_count==0,
                  mesh.joint_count?std::uint32_t(mesh.transforms.size()):1u,
                  optical);
    }
}

void draw(vk::CommandBuffer command, std::uint32_t image, const camera::frame &camera)
{
    auto &data = get();
    const auto &weapon_draws = weapon::update(camera.view);

    const auto extent = vk::swapchain::get()->extent;
    const float aspect = float(extent.width) / float(extent.height);
    const auto view_projection = perspective(camera.pose.fov, aspect, 0.1f, 1000.0f) * camera.view;
    const auto weapon_view_projection = weapon::view_projection(aspect, camera.view);

    cull_world(command,image,view_projection);
    if (data.snapshot) {
        const auto server_tick = connection::get()->server_tick.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();

        if (server_tick != data.last_animation_tick) {
            data.last_animation_tick = server_tick;
            data.animation_received = now;
        }

        const auto elapsed = std::chrono::duration<float>(now - data.animation_received).count();
        const float smooth_tick = float(server_tick) + std::clamp(elapsed, 0.f, 0.2f) * float(net::limits::tick_hz);

        data.skinned.clear();
        const bool needs_mirror_skin=has_optics() &&
            !planar_reflection(camera).has_value() &&
            !optical_rects(camera).empty();
        for (auto &mesh : data.meshes)
        {
            if (!mesh.joint_count || mesh.transforms.empty()) continue;
            std::vector<animation::matrix> matrices;
            matrices.reserve(mesh.instances.size() * mesh.joint_count);
            std::map<std::array<std::uint32_t, 5>, std::vector<animation::matrix>> sampled_poses;

            for (const auto &instance : mesh.instances)
            {
                const auto update = connection::animation(instance.id);
                const auto start = update ? update->start_tick : instance.animation_tick;
                const auto clip = update ? update->clip : instance.animation;
                const auto overlay = update ? update->overlay : instance.overlay_animation;
                const auto weight = update ? update->blend : instance.overlay_weight;
                const auto root = update ? update->mask_root : instance.overlay_root;

                const std::array<std::uint32_t, 5> key{
                    clip, start, overlay, root, std::bit_cast<std::uint32_t>(weight)
                };

                auto found = sampled_poses.find(key);

                if (found == sampled_poses.end()) {
                    const float time = (smooth_tick - float(start)) / float(net::limits::tick_hz);

                    std::vector<animation::matrix> bones;

                    if (!animation::sample(mesh.skin_id, clip, std::max(0.f, time), bones, overlay, weight, root) || bones.size() != mesh.joint_count)
                        throw std::runtime_error("Cannot sample streamed animation");
                    found = sampled_poses.emplace(key, std::move(bones)).first;
                }

                const auto &bones = found->second;
                matrices.insert(matrices.end(), bones.begin(), bones.end());
            }

            vk::buffer::write(mesh.frames.at(image).skin_palette, matrices.data(), matrices.size() * sizeof(matrices[0]));
            if(needs_mirror_skin)
                for(std::size_t k=0;k<mesh.instances.size();++k) {
                    const auto first=matrices.begin()+k*mesh.joint_count;
                    data.skinned.push_back({mesh.instances[k].id,
                        std::vector<animation::matrix>(first,first+mesh.joint_count)});
                }
        }
    }

    const auto local_mesh = [&](std::uint32_t id)
    {
        return std::find_if(data.local_meshes.begin(), data.local_meshes.end(),
                            [&](const gpu_mesh &mesh)
                            { return mesh.id == id; });
    };

    for (const auto &item : weapon_draws)
    {
        auto found = local_mesh(item.mesh_id);

        if (found == data.local_meshes.end())
            continue;
        auto &frame = found->frames.at(image);
        vk::buffer::write(frame.local_instances, item.instances.data(), item.instances.size() * sizeof(weapon::instance_data));

        if (!item.palette.empty())
            vk::buffer::write(frame.skin_palette, item.palette.data(), item.palette.size() * sizeof(animation::matrix));
    }
    draw_target(command, image, data.targets, [&](auto& graphics) {
        draw_world(graphics, image, view_projection);
        for (bool viewmodel : {false, true})
        {
            graphics.viewport(vk::Viewport{
                                  0, 0, float(extent.width), float(extent.height), viewmodel ? 0.f : .1f, viewmodel ? .099f : 1.f
                              },
                              vk::Rect2D{{0, 0}, extent});
            for (const auto &item : weapon_draws)
            {
                if (item.viewmodel != viewmodel)
                    continue;

                const auto found = local_mesh(item.mesh_id);

                if (found == data.local_meshes.end())
                    continue;
                const auto &mesh = *found;
                const auto &frame = mesh.frames.at(image);
                const auto &m = mesh.surface;

                const auto params = material_parameters(m, viewmodel ? weapon_view_projection : view_projection, mesh.joint_count);
                draw_mesh(graphics, mesh, frame, params, false, static_cast<std::uint32_t>(item.instances.size()));
            }
        }

    });
    // Transmissive faces use their own depth; opaque depth remains available behind glass.
    if (has_optics())
        draw_target(command, image, data.optical_targets, [&](auto& graphics) {
            draw_world(graphics, image, view_projection, {0, 0, 0, 1}, true);
        });
}

void draw_reflection(vk::CommandBuffer command, std::uint32_t image,
                     const camera::frame& original, const planar_view& planar)
{
    auto& data = get();
    if (!data.snapshot || optical_rects(original).empty()) return;
    const auto extent = vk::swapchain::get()->extent;
    const auto vp = perspective(planar.reflected.pose.fov, float(extent.width)/float(extent.height), .1f, 1000.f) * planar.reflected.view;
    cull_world(command, image, vp, true);
    draw_target(command, image, data.reflection_targets, [&](auto& graphics) {
        draw_world(graphics, image, vp, {planar.clip.x, planar.clip.y, planar.clip.z, planar.clip.w});
    });
}

void shutdown()
{
    auto &data = get();
    data.meshes.clear();
    release_local_frames();
    data.local_meshes.clear();
    weapon::shutdown();
    data.snapshot.reset();
    data.instance_sets.reset();
    release_frame_resources();
    data.cull.reset();
    texture::shutdown();

    if (data.instance_layout) {
        vk::device::get()->handle.destroyDescriptorSetLayout(data.instance_layout);
        data.instance_layout = nullptr;
    }
}

} // namespace clt::scene_renderer
