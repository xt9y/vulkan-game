#include "scene_renderer.hpp"

#include "clt/animation/animation.hpp"
#include "clt/network/connection.hpp"
#include "clt/render/texture.hpp"
#include <gpu/upload.hpp>
#include <gpu/culler.hpp>
#include <gpu/resource.hpp>
#include <gpu/descriptors.hpp>
#include <render/renderer.hpp>
#include <render/geometry.hpp>
#include <render/pipeline.hpp>
#include <render/target.hpp>
#include <render/attachments.hpp>
#include "clt/view/camera.hpp"
#include "clt/weapon/weapon.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/limits.hpp"
#include "srv/shared/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <bit>

namespace clt::scene_renderer
{
    namespace
    {
        using namespace vk::util::math;

        struct instance_data {
            mat4 model;
            mat4 normal;
        };

        struct draw_parameters {
            mat4 view_projection;
            std::array<float, 4> base_color;
            std::array<float, 4> surface;
            std::array<float, 4> emissive;
            std::uint32_t joint_count = 0;
        };
        static_assert(sizeof(draw_parameters) <= 128);

        struct gpu_mesh {
            std::uint32_t id = 0;
            ::scene::material surface;
            std::uint32_t skin_id = 0;
            std::uint32_t joint_count = 0;
            ::render::geometry geometry;
            gpu::owned_buffer instance_buffer;
            gpu::owned_buffer bounds_buffer;

            struct per_image {
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
            std::uint32_t index_count = 0;
        };

        static_assert(sizeof(instance_data) == 128);
        static_assert(sizeof(::scene::vertex) == 64);

        struct cull_parameters {
            std::array<vec4, 6> planes{};
            std::uint32_t count = 0;
            std::uint32_t index_count = 0;
        };
        static_assert(sizeof(cull_parameters) <= 128);

        struct state {
            std::shared_ptr<const ::scene::state> snapshot;
            std::vector<gpu_mesh> meshes;
            std::vector<gpu_mesh> local_meshes;
            std::unique_ptr<gpu::descriptors> local_sets;
            vk::DescriptorSetLayout instance_layout{};
            std::unique_ptr<gpu::descriptors> instance_sets;
            gpu::culler cull;
            std::uint32_t last_animation_tick = UINT32_MAX;
            std::chrono::steady_clock::time_point animation_received{};

            vk::RenderPass render_pass{};
            vk::PipelineLayout layout{};
            ::render::pipeline pipeline;

            struct target {
                ::render::attachments images;
                std::unique_ptr<::render::target> framebuffer;
            };

            std::vector<target> targets;

            std::array<std::string, 2> shaders;
        };

        state& get()
        {
            static state data;
            return data;
        }


        std::array<vk::DescriptorSetLayoutBinding,10> instance_bindings()
        {
            std::array<vk::DescriptorSetLayoutBinding, 10> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = (i < 4 || i == 9) ?
                    vk::DescriptorType::eStorageBuffer :
                    vk::DescriptorType::eCombinedImageSampler;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = i < 4 ?
                    vk::ShaderStageFlags(vk::ShaderStageFlagBits::eCompute) :
                    i == 9 ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eVertex) :
                    vk::ShaderStageFlags(vk::ShaderStageFlagBits::eFragment);
                if (i == 0)
                    bindings[i].stageFlags |= vk::ShaderStageFlagBits::eVertex;
            }


            return bindings;
        }

        // Shared render-system binding for streamed and local meshes.
        void draw_mesh(::render::renderer& graphics, const gpu_mesh& mesh,
                       const gpu_mesh::per_image& frame,
                       const draw_parameters& params, bool indirect,
                       std::uint32_t instances)
        {
            ::render::mesh geometry = mesh.geometry.view();
            geometry.streams.push_back({1,frame.visible.handle,0});

            ::render::material surface{};
            surface.pipeline = get().pipeline.handle();
            surface.layout = get().layout;
            surface.sets = {frame.set};
            surface.push_data = &params;
            surface.push_size = sizeof(params);
            surface.push_stages = vk::ShaderStageFlagBits::eVertex |
                                  vk::ShaderStageFlagBits::eFragment;

            if (indirect)
                graphics.draw_indirect(geometry, surface, frame.indirect.handle, 0);
            else
                graphics.draw(geometry, surface, instances);
        }

        draw_parameters material_parameters(const ::scene::material& m,
                                           const mat4& view_projection,
                                           std::uint32_t joints)
        {
            const auto flags =
                (m.base_color_texture ? 1u : 0u) |
                (m.metallic_roughness_texture ? 2u : 0u) |
                (m.normal_texture ? 4u : 0u) |
                (m.occlusion_texture ? 8u : 0u) |
                (m.emissive_texture ? 16u : 0u) |
                (m.alpha_mode << 5) |
                (m.procedural_pattern == 1u ? 128u : 0u);
            return {
                view_projection,m.base_color,
                {m.metallic,m.roughness,m.occlusion,m.alpha_cutoff},
                {m.emissive[0],m.emissive[1],m.emissive[2],float(flags)},
                joints
            };
        }

        void destroy_meshes(std::vector<gpu_mesh>& meshes)
        {
            for (auto& mesh : meshes)
            {
                mesh.geometry.reset();
                if (mesh.instance_buffer.handle)
                {
                    mesh.instance_buffer.reset();
                }
                if (mesh.bounds_buffer.handle)
                {
                    mesh.bounds_buffer.reset();
                }
                for (auto& frame : mesh.frames)
                {
                    if (frame.visible.handle) frame.visible.reset();
                    if (frame.indirect.handle) frame.indirect.reset();
                    if (frame.skin_palette.handle) frame.skin_palette.reset();
                    if (frame.local_instances.handle)
                    {
                        frame.local_instances.reset();
                    }
                }
            }

            meshes.clear();
        }

        struct plane {
            vec3 normal;
            float offset;
            float magnitude;
        };

        std::array<plane, 6> frustum(const mat4& m)
        {
            const auto make = [&](int row, float sign)
            {
                plane p{
                    {m(3, 0) + sign * m(row, 0),
                     m(3, 1) + sign * m(row, 1),
                     m(3, 2) + sign * m(row, 2)},
                    m(3, 3) + sign * m(row, 3), 0
                };
                p.magnitude = length(p.normal);
                return p;
            };
            plane near{
                {m(2, 0), m(2, 1), m(2, 2)}, m(2, 3), 0
            };
            near.magnitude = length(near.normal);
            return {make(0, 1), make(0, -1), make(1, 1), make(1, -1),
                    near, make(2, -1)};
        }

        void create_pipeline()
        {
            auto& data = get();
            vk::PushConstantRange range{};
            range.stageFlags = vk::ShaderStageFlagBits::eVertex |
                               vk::ShaderStageFlagBits::eFragment;
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
                {0, 0, vk::Format::eR32G32B32Sfloat,
                 offsetof(::scene::vertex, position)},
                {1, 0, vk::Format::eR32G32B32Sfloat,
                 offsetof(::scene::vertex, normal)},
                {2, 0, vk::Format::eR32G32Sfloat,
                 offsetof(::scene::vertex, u)},
                {3, 1, vk::Format::eR32Uint, 0},
                {4, 0, vk::Format::eR32G32B32A32Uint,
                 offsetof(::scene::vertex, joints)},
                {5, 0, vk::Format::eR32G32B32A32Sfloat,
                 offsetof(::scene::vertex, weights)}
            };
            raster.blend_attachments.resize(3);
            for (auto& blend : raster.blend_attachments)
                blend.colorWriteMask = vk::ColorComponentFlagBits::eR |
                                       vk::ColorComponentFlagBits::eG |
                                       vk::ColorComponentFlagBits::eB |
                                       vk::ColorComponentFlagBits::eA;

            data.pipeline.create(raster, {
                {vk::ShaderStageFlagBits::eVertex, data.shaders[0]},
                {vk::ShaderStageFlagBits::eFragment, data.shaders[1]}
            });
        }

        std::array<vk::Format, 3> color_formats()
        {
            const auto precision = game::options::get()->graphics.gbuffer;
            const auto format = precision == game::options::gbuffer_precision::rgba16f
                ? vk::Format::eR16G16B16A16Sfloat : vk::Format::eR8G8B8A8Unorm;
            return {format, format, format};
        }

        vk::Format depth_format()
        {
            const auto precision = game::options::get()->graphics.depth;
            const auto format = precision == game::options::depth_precision::d32
                ? vk::Format::eD32Sfloat : vk::Format::eD16Unorm;
            const auto properties = vk::physical_device::get()->handle
                .getFormatProperties(format);
            const auto required = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                vk::FormatFeatureFlagBits::eSampledImage;
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("Configured depth format unsupported");
            return format;
        }

        void create_render_pass()
        {
            auto& data = get();
            std::array<vk::AttachmentDescription, 4> attachments{};
            std::array<vk::AttachmentReference, 3> colors{};

            for (std::uint32_t i = 0; i < colors.size(); ++i)
            {
                attachments[i].format = color_formats()[i];
                attachments[i].samples = vk::SampleCountFlagBits::e1;
                attachments[i].loadOp = vk::AttachmentLoadOp::eClear;
                attachments[i].storeOp = vk::AttachmentStoreOp::eStore;
                attachments[i].initialLayout = vk::ImageLayout::eUndefined;
                attachments[i].finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                colors[i] = {i, vk::ImageLayout::eColorAttachmentOptimal};
            }

            attachments[3].format = depth_format();
            attachments[3].samples = vk::SampleCountFlagBits::e1;
            attachments[3].loadOp = vk::AttachmentLoadOp::eClear;
            attachments[3].storeOp = vk::AttachmentStoreOp::eStore;
            attachments[3].initialLayout = vk::ImageLayout::eUndefined;
            attachments[3].finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;

            vk::AttachmentReference depth{3, vk::ImageLayout::eDepthStencilAttachmentOptimal};
            vk::SubpassDescription subpass{};
            subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
            subpass.colorAttachmentCount = colors.size();
            subpass.pColorAttachments = colors.data();
            subpass.pDepthStencilAttachment = &depth;

            std::array<vk::SubpassDependency, 2> barriers{};
            barriers[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            barriers[0].dstSubpass = 0;
            barriers[0].srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
            barriers[0].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eEarlyFragmentTests;
            barriers[0].dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            barriers[1].srcSubpass = 0;
            barriers[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            barriers[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                vk::PipelineStageFlagBits::eLateFragmentTests;
            barriers[1].dstStageMask = vk::PipelineStageFlagBits::eComputeShader;
            barriers[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;

            vk::RenderPassCreateInfo info{};
            info.attachmentCount = attachments.size();
            info.pAttachments = attachments.data();
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = barriers.size();
            info.pDependencies = barriers.data();
            data.render_pass = vk::render_pass::create(info);
        }

        void create_target()
        {
            auto& data=get();
            const auto extent=vk::swapchain::get()->extent;
            const auto formats=color_formats();
            state::target target{};
            target.images.create(extent,
                std::vector<vk::Format>(formats.begin(),formats.end()),depth_format());
            target.framebuffer=std::make_unique<::render::target>();
            target.framebuffer->create_framebuffer(
                data.render_pass,
                {target.images.color(0),target.images.color(1),
                 target.images.color(2),target.images.depth()},
                extent);
            data.targets.push_back(std::move(target));
        }
    } // namespace

    void init(const char* vertex_path, const char* fragment_path)
    {
        if (!vertex_path || !fragment_path || !*vertex_path || !*fragment_path)
        {
            throw std::invalid_argument("Scene shader paths must not be empty");
        }

        auto& data = get();
        data.shaders = {vertex_path, fragment_path};

        const auto bindings=instance_bindings();
        const auto device = vk::device::get()->handle;
        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        data.instance_layout = device.createDescriptorSetLayout(info);
        texture::init();
        weapon::initialize();
        texture::pin(weapon::assets());
        texture::synchronize(::scene::state{});

        data.cull.create(game::options::get()->graphics.culling_shader,
                         data.instance_layout);
    }

    gbuffer outputs(std::uint32_t image)
    {
        const auto& target = get().targets.at(image);
        return {{
            target.images.color(0),target.images.color(1),
            target.images.color(2),target.images.depth()
        },vk::swapchain::get()->extent};
    }

    void release_local_frames()
    {
        auto& data = get();
        for (auto& mesh : data.local_meshes)
        {
            for (auto& frame : mesh.frames)
            {
                if (frame.local_instances.handle)
                {
                    frame.local_instances.reset();
                }
                if (frame.visible.handle)
                {
                    frame.visible.reset();
                }
                if (frame.skin_palette.handle)
                {
                    frame.skin_palette.reset();
                }
            }
            mesh.frames.clear();
        }
        data.local_sets.reset();
    }

    void create_local_frames()
    {
        auto& data = get();
        const auto& source = weapon::assets();
        if (source.meshes.empty())
        {
            return;
        }
        if (data.local_meshes.empty())
        {
            gpu::upload::batch transfers;
            for (const auto& mesh : source.meshes)
            {
                gpu_mesh output;
                output.id = mesh.id;
                output.skin_id = mesh.skin_id;
                output.joint_count = animation::joints(mesh.skin_id);
                output.surface = mesh.surface;
                output.index_count = mesh.indices.size();
                output.geometry.upload(transfers,
                    mesh.vertices.data(),
                    mesh.vertices.size() * sizeof(::scene::vertex),
                    mesh.indices.data(),
                    mesh.indices.size() * sizeof(std::uint32_t),
                    output.index_count);
                data.local_meshes.push_back(std::move(output));
            }
            transfers.finish();
        }
        const auto count=std::uint32_t(
            data.local_meshes.size()*vk::swapchain::get()->images.size());
        const auto schema=instance_bindings();
        const auto device=vk::device::get()->handle;
        data.local_sets=std::make_unique<gpu::descriptors>();
        data.local_sets->create(std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(),schema.end()),count);
        std::uint32_t descriptor_index=0;
        std::array<std::uint32_t, 192> identities{};
        for (std::uint32_t i = 0; i < identities.size(); ++i)
        {
            identities[i] = i;
        }
        for (auto& mesh : data.local_meshes)
        {
            mesh.frames.resize(vk::swapchain::get()->images.size());
            for (auto& frame : mesh.frames)
            {
                const auto memory = vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent;
                frame.local_instances = gpu::create_buffer(
                    192 * sizeof(instance_data), vk::BufferUsageFlagBits::eStorageBuffer, memory
                );
                frame.visible = gpu::create_buffer(
                    sizeof(identities),
                    vk::BufferUsageFlagBits::eVertexBuffer |
                        vk::BufferUsageFlagBits::eStorageBuffer,
                    memory
                );
                vk::buffer::write(frame.visible, identities.data(), sizeof(identities));
                frame.skin_palette = gpu::create_buffer(
                    std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix),
                    vk::BufferUsageFlagBits::eStorageBuffer,
                    memory
                );
                frame.set=data.local_sets->set(descriptor_index++);
                std::array<vk::DescriptorBufferInfo, 5> infos{};
                std::array<vk::WriteDescriptorSet, 5> writes{};
                for (std::uint32_t i = 0; i < 5; ++i)
                {
                    infos[i].buffer =
                        i == 4 ? frame.skin_palette.handle : frame.local_instances.handle;
                    infos[i].range = VK_WHOLE_SIZE;
                    writes[i].dstSet = frame.set;
                    writes[i].dstBinding = i == 4 ? 9 : i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                    writes[i].pBufferInfo = &infos[i];
                }
                device.updateDescriptorSets(writes, {});
                texture::bind(frame.set, mesh.surface);
            }
        }
    }

    void release_frame_resources()
    {
        auto& data = get();
        release_local_frames();
        // Keep immutable scene geometry across swapchain recreation.
        // Only the per-image culling, skinning and descriptor data changes.
        for(auto& mesh : data.meshes)
            mesh.frames.clear();
        data.instance_sets.reset();
        data.pipeline.reset();
        vk::pipeline_layout::destroy(data.layout);
        data.layout = nullptr;

        // Framebuffer handles must die before their attachment image views.
        data.targets.clear();
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

    void append_instance(gpu_mesh& mesh, const ::scene::instance& instance)
    {
        const auto& pose = instance.pose;
        const vec3 center{
            mesh.center.x * pose.scale.x,
            mesh.center.y * pose.scale.y,
            mesh.center.z * pose.scale.z
        };
        const vec3 inverse{
            pose.scale.x ? 1.0f / pose.scale.x : 0.0f,
            pose.scale.y ? 1.0f / pose.scale.y : 0.0f,
            pose.scale.z ? 1.0f / pose.scale.z : 0.0f
        };
        const float max_scale = std::max({
            std::abs(pose.scale.x), std::abs(pose.scale.y), std::abs(pose.scale.z)
        });
        mesh.instances.push_back(instance);
        mesh.transforms.push_back({
            trs(pose.position, pose.rotation, pose.scale),
            rotation(pose.rotation) * scale(inverse)
        });
        const auto position = pose.position + rotate(pose.rotation, center);
        mesh.bounds.push_back({
            position.x, position.y, position.z, mesh.radius * max_scale
        });
    }

    void bind_instances(gpu_mesh& mesh)
    {
        if (!mesh.instance_buffer.handle || !mesh.bounds_buffer.handle)
        {
            return;
        }

        for (auto& frame : mesh.frames)
        {
            if (!frame.set || !frame.visible.handle || !frame.indirect.handle)
            {
                continue;
            }
            const std::array<vk::Buffer, 4> buffers{
                mesh.instance_buffer.handle,
                mesh.bounds_buffer.handle,
                frame.visible.handle,
                frame.indirect.handle
            };
            std::array<vk::DescriptorBufferInfo, 4> infos{};
            std::array<vk::WriteDescriptorSet, 4> writes{};

            for (std::uint32_t i = 0; i < buffers.size(); ++i)
            {
                infos[i].buffer = buffers[i];
                infos[i].offset = 0;
                infos[i].range = VK_WHOLE_SIZE;
                writes[i].dstSet = frame.set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                writes[i].pBufferInfo = &infos[i];
            }
            vk::device::get()->handle.updateDescriptorSets(writes, {});
            vk::DescriptorBufferInfo palette_info{};
            palette_info.buffer = frame.skin_palette.handle;
            palette_info.range = VK_WHOLE_SIZE;
            vk::WriteDescriptorSet palette_write{};
            palette_write.dstSet = frame.set;
            palette_write.dstBinding = 9;
            palette_write.descriptorCount = 1;
            palette_write.descriptorType = vk::DescriptorType::eStorageBuffer;
            palette_write.pBufferInfo = &palette_info;
            vk::device::get()->handle.updateDescriptorSets({palette_write}, {});
            texture::bind(frame.set, mesh.surface);
        }
    }

    void refresh_instances(gpu_mesh& mesh, std::size_t first = 0,
                           gpu::upload::batch* shared_upload = nullptr)
    {
        if (mesh.transforms.empty())
        {
            return;
        }

        gpu::upload::batch local_upload;
        auto& transfers=shared_upload?*shared_upload:local_upload;
        const auto transform_bytes = mesh.transforms.size() * sizeof(instance_data);
        if (!mesh.instance_buffer.handle || mesh.instance_buffer.size < transform_bytes)
        {
            auto replacement = transfers.add(
                mesh.transforms.data(), transform_bytes,
                vk::BufferUsageFlagBits::eStorageBuffer,
                std::max<vk::DeviceSize>(transform_bytes, mesh.instance_buffer.size * 2)
            );
            if (mesh.instance_buffer.handle) mesh.instance_buffer.reset();
            mesh.instance_buffer = replacement;
        }
        else if (first < mesh.transforms.size())
        {
            transfers.write(
                mesh.instance_buffer, mesh.transforms.data() + first,
                (mesh.transforms.size() - first) * sizeof(instance_data),
                first * sizeof(instance_data)
            );
        }

        const auto visible_bytes = mesh.bounds.size() * sizeof(std::uint32_t);
        const auto count = vk::swapchain::get()->images.size();
        mesh.frames.resize(count);
        for (auto& frame : mesh.frames)
        {
            if (!frame.visible.handle || frame.visible.size < visible_bytes)
            {
                auto replacement = gpu::create_buffer(
                    std::max<vk::DeviceSize>(visible_bytes, frame.visible.size * 2),
                    vk::BufferUsageFlagBits::eVertexBuffer |
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                if (frame.visible.handle) frame.visible.reset();
                frame.visible = std::move(replacement);
            }

            if (!frame.indirect.handle)
            {
                frame.indirect = gpu::create_buffer(
                    sizeof(vk::DrawIndexedIndirectCommand),
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst |
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
            }
        }

        const auto palette_size = std::max<std::size_t>(
            sizeof(animation::matrix), mesh.instances.size() *
                std::max<std::uint32_t>(1, mesh.joint_count) * sizeof(animation::matrix));
        for (auto& frame : mesh.frames)
        {
            if (!frame.skin_palette.handle || frame.skin_palette.size < palette_size)
            {
                if (frame.skin_palette.handle) frame.skin_palette.reset();
                frame.skin_palette = gpu::create_buffer(
                    palette_size, vk::BufferUsageFlagBits::eStorageBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent);
            }
        }
        const auto size = mesh.bounds.size() * sizeof(mesh.bounds.front());
        if (!mesh.bounds_buffer.handle || mesh.bounds_buffer.size < size)
        {
            auto replacement = transfers.add(
                mesh.bounds.data(), size, vk::BufferUsageFlagBits::eStorageBuffer,
                std::max<vk::DeviceSize>(size, mesh.bounds_buffer.size * 2)
            );
            if (mesh.bounds_buffer.handle) mesh.bounds_buffer.reset();
            mesh.bounds_buffer = replacement;
        }
        else if (first < mesh.bounds.size())
        {
            transfers.write(
                mesh.bounds_buffer, mesh.bounds.data() + first,
                (mesh.bounds.size() - first) * sizeof(mesh.bounds.front()),
                first * sizeof(mesh.bounds.front())
            );
        }

        if(!shared_upload)transfers.finish();
        bind_instances(mesh);
    }

    void rebuild_instance_frames()
    {
        auto& data = get();
        if(data.meshes.empty()) return;

        const auto count = std::max<std::size_t>(1, data.meshes.size()) *
            std::max<std::size_t>(1, vk::swapchain::get()->images.size());
        const auto schema = instance_bindings();
        data.instance_sets = std::make_unique<gpu::descriptors>();
        data.instance_sets->create(
            std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(), schema.end()),
            static_cast<std::uint32_t>(count));

        std::uint32_t index = 0;
        gpu::upload::batch transfers;
        for(auto& mesh : data.meshes)
        {
            if(mesh.transforms.empty()) continue;
            // Existing instance and geometry buffers remain GPU-resident.
            refresh_instances(mesh, mesh.transforms.size(),&transfers);
            for(auto& frame : mesh.frames)
                frame.set = data.instance_sets->set(index++);
            bind_instances(mesh);
        }
        transfers.finish();
    }

    bool needs_sync()
    {
        return connection::snapshot() != get().snapshot;
    }

    void synchronize()
    {
        auto& data = get();
        const auto source = connection::snapshot();

        if (source == data.snapshot)
        {
            return;
        }

        // Scene batches are append-only; reuse immutable GPU meshes and grow
        // only the buffers containing newly generated instances.
        bool appended = data.snapshot && source &&
            source->textures.size() >= data.snapshot->textures.size() &&
            source->meshes.size() == data.snapshot->meshes.size() &&
            source->instances.size() >= data.snapshot->instances.size();

        if (appended)
        {
            for (std::size_t i = 0; i < source->meshes.size(); ++i)
            {
                const auto& a = source->meshes[i];
                const auto& b = data.snapshot->meshes[i];
                if (!::scene::same_mesh(a, b))
                {
                    appended = false;
                    break;
                }
            }
        }

        if (appended)
        {
            appended = std::equal(
                data.snapshot->instances.begin(), data.snapshot->instances.end(),
                source->instances.begin(), ::scene::same_instance
            );
        }

        if (appended)
        {
            animation::synchronize(*source);
            if (source->textures.size() != data.snapshot->textures.size())
                texture::synchronize(*source);
            std::vector<std::size_t> previous_counts;
            previous_counts.reserve(data.meshes.size());
            for (const auto& mesh : data.meshes)
            {
                previous_counts.push_back(mesh.transforms.size());
            }

            std::unordered_map<std::uint32_t,std::size_t> mesh_indices;
            mesh_indices.reserve(data.meshes.size());
            for(std::size_t j=0;j<data.meshes.size();++j)
                mesh_indices.emplace(data.meshes[j].id,j);
            for(std::size_t i=data.snapshot->instances.size();
                i<source->instances.size();++i)
            {
                const auto found=mesh_indices.find(source->instances[i].mesh_id);
                if(found!=mesh_indices.end())
                    append_instance(data.meshes[found->second],source->instances[i]);
            }
            gpu::upload::batch scene_upload;
            for (std::size_t i = 0; i < data.meshes.size(); ++i)
            {
                if (data.meshes[i].transforms.size() != previous_counts[i])
                {
                    refresh_instances(data.meshes[i], previous_counts[i],&scene_upload);
                }
            }
            scene_upload.finish();
            data.snapshot = source;
            return;
        }

        std::vector<gpu_mesh> meshes;
        std::unique_ptr<gpu::descriptors> new_sets;

        if (source)
        {
            animation::synchronize(*source);
            texture::synchronize(*source);
        }
        else
        {
            animation::reset();
            texture::reset();
            texture::synchronize(::scene::state{});
        }

        if (source)
        {
            meshes.reserve(source->meshes.size());

            gpu::upload::batch transfers;
            try
            {
                for (const auto& mesh : source->meshes)
                {
                    gpu_mesh result;
                    result.id = mesh.id;
                    result.surface = mesh.surface;
                    result.skin_id = mesh.skin_id;
                    result.joint_count = animation::joints(mesh.skin_id);
                    result.index_count =
                        static_cast<std::uint32_t>(mesh.indices.size());

                    try
                    {
                        const auto vertex_bytes =
                            mesh.vertices.size() * sizeof(::scene::vertex);

                        const auto index_bytes =
                            mesh.indices.size() * sizeof(std::uint32_t);

                        result.geometry.upload(transfers,
                            mesh.vertices.data(),vertex_bytes,
                            mesh.indices.data(),index_bytes,result.index_count);

                        if (!mesh.vertices.empty())
                        {
                            vec3 low = mesh.vertices.front().position;
                            vec3 high = low;
                            for (const auto& vertex : mesh.vertices)
                            {
                                const auto p = vertex.position;
                                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                            }
                            result.center = (low + high) * 0.5f;
                            result.radius = length((high - low) * 0.5f);
                        }

                        meshes.push_back(std::move(result));
                    }
                    catch (...)
                    {
                        // The geometry owner releases both buffers on unwinding.
                        destroy_meshes(meshes);
                        throw;
                    }
                }
                transfers.finish();
            }
            catch (...)
            {
                destroy_meshes(meshes);
                throw;
            }
        }

        try
        {
            if (source)
            {
                std::unordered_map<std::uint32_t,std::size_t> mesh_indices;
                mesh_indices.reserve(meshes.size());
                for(std::size_t j=0;j<meshes.size();++j)
                    mesh_indices.emplace(meshes[j].id,j);
                for(const auto& instance:source->instances)
                {
                    const auto found=mesh_indices.find(instance.mesh_id);
                    if(found==mesh_indices.end())continue;
                    auto& mesh=meshes[found->second];
                    if(mesh.index_count)append_instance(mesh,instance);
                }

                // Allocate each scene descriptor through the reusable GPU
                // descriptor owner, using a layout compatible with the renderer.
                const auto count=std::max<std::size_t>(1,meshes.size()) *
                    std::max<std::size_t>(1,vk::swapchain::get()->images.size());
                const auto schema=instance_bindings();
                new_sets=std::make_unique<gpu::descriptors>();
                new_sets->create(std::vector<vk::DescriptorSetLayoutBinding>(schema.begin(),schema.end()),
                    static_cast<std::uint32_t>(count));
                std::uint32_t descriptor_index=0;
                gpu::upload::batch instances_upload;

                for (auto& mesh : meshes)
                {
                    if (mesh.transforms.empty())
                    {
                        continue;
                    }

                    refresh_instances(mesh,0,&instances_upload);

                    for (auto& frame : mesh.frames)
                    {
                        frame.set=new_sets->set(descriptor_index++);
                    }
                    bind_instances(mesh);
                }
                instances_upload.finish();
            }
        }
        catch (...)
        {
            destroy_meshes(meshes);
            new_sets.reset();
            throw;
        }

        
        destroy_meshes(data.meshes);
        data.instance_sets=std::move(new_sets);
        data.meshes = std::move(meshes);
        data.snapshot = source;
        for (auto& mesh : data.local_meshes)
        {
            for (auto& frame : mesh.frames)
            {
                texture::bind(frame.set, mesh.surface);
            }
        }
    }

    void draw(vk::CommandBuffer command, std::uint32_t image,
              const camera::frame& camera)
    {
        auto& data = get();
        const auto& weapon_draws = weapon::update(camera.view);
        const auto extent = vk::swapchain::get()->extent;
        const float aspect = float(extent.width) / float(extent.height);
        const auto view_projection =
            perspective(camera.pose.fov, aspect, 0.1f, 1000.0f) * camera.view;
        const auto weapon_view_projection = weapon::view_projection(aspect, camera.view);

        std::array<vk::ClearValue, 4> clear{};
        for (std::size_t i = 0; i < 3; ++i)
        {
            clear[i].color = vk::ClearColorValue(
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
            );
        }
        clear[3].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

        vk::RenderPassBeginInfo begin{};
        begin.renderPass = data.render_pass;
        begin.framebuffer = data.targets.at(image).framebuffer->framebuffer();
        begin.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        begin.clearValueCount = clear.size();
        begin.pClearValues = clear.data();

        // Compute visibility and compact instance IDs for indirect graphics draws.
        if (data.snapshot)
        {
            const auto camera_planes = frustum(view_projection);
            cull_parameters params;
            for (std::size_t i = 0; i < params.planes.size(); ++i)
            {
                const auto& plane = camera_planes[i];
                const float inverse = 1.0f / std::max(plane.magnitude, 1e-6f);
                params.planes[i] = {
                    plane.normal.x * inverse,
                    plane.normal.y * inverse,
                    plane.normal.z * inverse,
                    plane.offset * inverse
                };
            }

            std::vector<vk::Buffer> indirects;
            indirects.reserve(data.meshes.size());
            for (const auto& mesh : data.meshes)
                if (!mesh.transforms.empty())
                    indirects.push_back(mesh.frames.at(image).indirect.handle);
            data.cull.begin(command, indirects);
            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty()) continue;
                data.cull.dispatch(
                    command, mesh.frames.at(image).set, params.planes,
                    static_cast<std::uint32_t>(mesh.transforms.size()),
                    mesh.index_count
                );
            }
            data.cull.end(command);


        }

        // Write per-swapchain-image palettes only after its frame fence completes.
        if (data.snapshot)
        {
            const auto server_tick = connection::get()->server_tick.load(
                std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (server_tick != data.last_animation_tick)
            {
                data.last_animation_tick = server_tick;
                data.animation_received = now;
            }
            const auto elapsed = std::chrono::duration<float>(
                now - data.animation_received).count();
            const float smooth_tick = float(server_tick) + std::clamp(elapsed, 0.f, 0.2f) * float(net::limits::tick_hz);
            for (auto& mesh : data.meshes)
            {
                if (!mesh.joint_count || mesh.transforms.empty()) continue;
                std::vector<animation::matrix> matrices;
                matrices.reserve(mesh.instances.size() * mesh.joint_count);
                // Multiple instances often use the same clip and start tick.
                // Sample that pose once per frame; retain each instance's
                // palette order and the same exact animation time.
                std::map<std::array<std::uint32_t,5>,
                         std::vector<animation::matrix>> sampled_poses;
                for (const auto& instance : mesh.instances)
                {
                    const auto update = connection::animation(instance.id);
                    const auto start = update ? update->start_tick : instance.animation_tick;
                    const auto clip = update ? update->clip : instance.animation;
                    const auto overlay = update ? update->overlay : instance.overlay_animation;
                    const auto weight = update ? update->blend : instance.overlay_weight;
                    const auto root = update ? update->mask_root : instance.overlay_root;
                    const std::array<std::uint32_t,5> key{
                        clip,start,overlay,root,std::bit_cast<std::uint32_t>(weight)
                    };
                    auto found=sampled_poses.find(key);
                    if(found==sampled_poses.end())
                    {
                        const float time=(smooth_tick-float(start))/
                            float(net::limits::tick_hz);
                        std::vector<animation::matrix> bones;
                        if(!animation::sample(mesh.skin_id,clip,std::max(0.f,time),
                            bones,overlay,weight,root) ||
                            bones.size()!=mesh.joint_count)
                            throw std::runtime_error("Cannot sample streamed animation");
                        found=sampled_poses.emplace(key,std::move(bones)).first;
                    }
                    const auto& bones=found->second;
                    matrices.insert(matrices.end(),bones.begin(),bones.end());
                }
                vk::buffer::write(mesh.frames.at(image).skin_palette,
                                  matrices.data(), matrices.size() * sizeof(matrices[0]));
            }
        }

        for (const auto& item : weapon_draws)
        {
            auto found = std::find_if(
                data.local_meshes.begin(),
                data.local_meshes.end(),
                [&](const gpu_mesh& mesh)
                {
                    return mesh.id == item.mesh_id;
                }
            );
            if (found == data.local_meshes.end())
            {
                continue;
            }
            auto& frame = found->frames.at(image);
            vk::buffer::write(
                frame.local_instances,
                item.instances.data(),
                item.instances.size() * sizeof(weapon::instance_data)
            );
            if (!item.palette.empty())
            {
                vk::buffer::write(
                    frame.skin_palette,
                    item.palette.data(),
                    item.palette.size() * sizeof(animation::matrix)
                );
            }
        }
        ::render::renderer graphics(command);
        graphics.begin_pass(begin);
        graphics.viewport(
            vk::Viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.1f, 1.0f},
            vk::Rect2D{{0, 0}, extent});

        if (data.snapshot)
        {
            for (const auto& mesh : data.meshes)
            {
                if (mesh.transforms.empty())
                {
                    continue;
                }

                const auto& m = mesh.surface;
                const auto params=material_parameters(m,view_projection,mesh.joint_count);
                draw_mesh(graphics, mesh, mesh.frames.at(image), params, true, 1);
            }
        }

        // Disjoint depth ranges give the viewmodel independent depth testing while
        // preserving world depth for deferred HDR lighting and sky classification.
        // Draw both world-attached weapons and the first-person viewmodel.
        for (bool viewmodel : {false, true})
        {
            graphics.viewport(
                vk::Viewport{
                    0, 0, float(extent.width), float(extent.height),
                    viewmodel ? 0.f : .1f, viewmodel ? .099f : 1.f
                },
                vk::Rect2D{{0, 0}, extent});
            for (const auto& item : weapon_draws)
            {
                if (item.viewmodel != viewmodel)
                {
                    continue;
                }
                const auto found = std::find_if(
                    data.local_meshes.begin(),
                    data.local_meshes.end(),
                    [&](const gpu_mesh& mesh)
                    {
                        return mesh.id == item.mesh_id;
                    }
                );
                if (found == data.local_meshes.end())
                {
                    continue;
                }
                const auto& mesh = *found;
                const auto& frame = mesh.frames.at(image);
                const auto& m = mesh.surface;
                const auto params=material_parameters(m,
                    viewmodel?weapon_view_projection:view_projection,mesh.joint_count);
                draw_mesh(graphics, mesh, frame, params, false,
                          static_cast<std::uint32_t>(item.instances.size()));
            }
        }

        graphics.end_pass();
    }

    void shutdown()
    {
        auto& data = get();

        destroy_meshes(data.meshes);
        release_local_frames();
        destroy_meshes(data.local_meshes);
        weapon::shutdown();
        data.snapshot.reset();
        data.instance_sets.reset();

        release_frame_resources();

        data.cull.reset();
        texture::shutdown();
        if (data.instance_layout)
        {
            vk::device::get()->handle.destroyDescriptorSetLayout(data.instance_layout);
            data.instance_layout = nullptr;
        }
    }
} // namespace clt::scene_renderer
