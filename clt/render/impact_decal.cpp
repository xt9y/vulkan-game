#include "impact_decal.hpp"

#include "scene_renderer.hpp"
#include "texture.hpp"
#include "clt/weapon/impact.hpp"
#include "clt/weapon/weapon.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace clt::impact_decal
{
    namespace
    {
        using namespace vk::util::math;
        constexpr std::uint32_t tile_size = 16;
        constexpr std::uint32_t tile_capacity = 64;
        constexpr std::uint32_t max_decals = 2112;

        struct alignas(16) gpu_decal
        {
            mat4 world_to_local;
            vec4 normal;
            vec4 tint;
        };
        static_assert(sizeof(gpu_decal) == 96);

        struct push
        {
            mat4 inverse_view_projection;
            std::uint32_t width = 0, height = 0, count = 0;
            float depth_min = .1f;
        };
        static_assert(sizeof(push) == 80);

        struct target
        {
            vk::image::resource overlay{};
            vk::ImageView overlay_view{};
            vk::buffer::resource decals{}, tile_counts{}, tile_indices{};
            vk::DescriptorSet set{};
            bool initialized = false;
        };

        struct state
        {
            std::vector<target> targets;
            vk::Sampler sampler{};
            vk::DescriptorSetLayout descriptor_layout{};
            vk::DescriptorPool pool{};
            vk::PipelineLayout layout{};
            vk::Pipeline pipeline{};
        };

        state& get()
        {
            static state data;
            return data;
        }

        mat4 inverse(mat4 input)
        {
            double a[4][8]{};
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c) a[r][c] = input(r, c);
                a[r][r + 4] = 1;
            }

            for (int c = 0; c < 4; ++c)
            {
                int pivot = c;
                for (int r = c + 1; r < 4; ++r)
                    if (std::abs(a[r][c]) > std::abs(a[pivot][c]))
                        pivot = r;
                if (std::abs(a[pivot][c]) < 1e-14)
                    throw std::runtime_error("Singular view-projection");
                if (pivot != c)
                    for (int k = 0; k < 8; ++k) std::swap(a[pivot][k], a[c][k]);
                const auto divisor = a[c][c];
                for (int k = 0; k < 8; ++k) a[c][k] /= divisor;
                for (int r = 0; r < 4; ++r)
                {
                    if (r == c) continue;
                    const auto factor = a[r][c];
                    for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
                }
            }
            mat4 result{};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    result(r, c) = float(a[r][c + 4]);
            return result;
        }

        gpu_decal convert(const impact::projector& source)
        {
            gpu_decal decal{};
            const auto& m = source.transform;
            const vec3 p{m(0, 3), m(1, 3), m(2, 3)};
            decal.world_to_local = mat4::identity();

            for (int axis = 0; axis < 3; ++axis)
            {
                const vec3 column{m(0, axis), m(1, axis), m(2, axis)};
                const float denominator = std::max(dot(column, column), 1e-12f);
                for (int c = 0; c < 3; ++c)
                    decal.world_to_local(axis, c) = (&column.x)[c] / denominator;
                decal.world_to_local(axis, 3) = -dot(column, p) / denominator;
            }
            const vec3 direction{m(0, 2), m(1, 2), m(2, 2)};
            const vec3 normal = normalize(direction);
            decal.normal = {
                normal.x, normal.y, normal.z,
                float(source.variant + (source.growing_pool ? 8u : 0u))
            };
            decal.tint = {1.f, 1.f, 1.f, source.wetness};
            return decal;
        }

        void index(
            const impact::projector& decal, const mat4& vp,
            std::uint32_t width, std::uint32_t height,
            std::vector<std::uint32_t>& counts,
            std::vector<std::uint32_t>& indices,
            std::uint32_t id)
        {
            const std::uint32_t columns = (width + tile_size - 1) / tile_size;
            const std::uint32_t rows = (height + tile_size - 1) / tile_size;
            float min_x = float(width), min_y = float(height);
            float max_x = 0, max_y = 0;
            bool crosses_near = false;

            for (const float x : {-.5f, .5f})
                for (const float y : {-.5f, .5f})
                    for (const float z : {-.18f, .18f})
                    {
                        const auto world = decal.transform * vec4{x, y, z, 1};
                        const auto clip = vp * world;
                        if (clip.w <= .0001f)
                        {
                            crosses_near = true;
                            continue;
                        }
                        const float sx = (clip.x / clip.w * .5f + .5f) * width;
                        const float sy = (clip.y / clip.w * .5f + .5f) * height;
                        min_x = std::min(min_x, sx);
                        max_x = std::max(max_x, sx);
                        min_y = std::min(min_y, sy);
                        max_y = std::max(max_y, sy);
                    }

            if (crosses_near)
            {
                min_x = min_y = 0;
                max_x = float(width);
                max_y = float(height);
            }
            if (max_x < 0 || max_y < 0 ||
                min_x >= float(width) || min_y >= float(height))
                return;

            const auto x0 = std::uint32_t(std::clamp(int(std::floor(min_x / tile_size)),
                                                 0, int(columns) - 1));
            const auto y0 = std::uint32_t(std::clamp(int(std::floor(min_y / tile_size)),
                                                 0, int(rows) - 1));
            const auto x1 = std::uint32_t(std::clamp(int(std::floor(max_x / tile_size)),
                                                 0, int(columns) - 1));
            const auto y1 = std::uint32_t(std::clamp(int(std::floor(max_y / tile_size)),
                                                 0, int(rows) - 1));

            for (std::uint32_t y = y0; y <= y1; ++y)
                for (std::uint32_t x = x0; x <= x1; ++x)
                {
                    const auto tile = std::size_t(y) * columns + x;
                    if (counts[tile] >= tile_capacity) continue;
                    indices[tile * tile_capacity + counts[tile]++] = id;
                }
        }

        void write_target(target& t, vk::ImageView depth, vk::ImageView normal)
        {
            const auto device = vk::device::get()->handle;
            std::array<vk::DescriptorImageInfo, 3> images{};
            images[0] = {get().sampler, depth,
                vk::ImageLayout::eDepthStencilReadOnlyOptimal};
            images[1] = {get().sampler, normal,
                vk::ImageLayout::eShaderReadOnlyOptimal};
            images[2].imageView = t.overlay_view;
            images[2].imageLayout = vk::ImageLayout::eGeneral;
            std::array<vk::WriteDescriptorSet, 6> writes{};
            for (std::uint32_t i = 0; i < 3; ++i)
            {
                writes[i].dstSet = t.set;
                writes[i].dstBinding = i == 2 ? 7 : i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = i == 2 ?
                    vk::DescriptorType::eStorageImage :
                    vk::DescriptorType::eCombinedImageSampler;
                writes[i].pImageInfo = &images[i];
            }
            std::array<vk::DescriptorBufferInfo, 3> buffers{};
            const std::array<vk::Buffer, 3> handles{
                t.decals.handle, t.tile_counts.handle, t.tile_indices.handle
            };
            for (std::uint32_t i = 0; i < 3; ++i)
            {
                buffers[i].buffer = handles[i];
                buffers[i].range = VK_WHOLE_SIZE;
                writes[i + 3].dstSet = t.set;
                writes[i + 3].dstBinding = i + 8;
                writes[i + 3].descriptorCount = 1;
                writes[i + 3].descriptorType = vk::DescriptorType::eStorageBuffer;
                writes[i + 3].pBufferInfo = &buffers[i];
            }
            device.updateDescriptorSets(writes, {});
        }

        void bind_atlas(vk::DescriptorSet set)
        {
            std::array<vk::DescriptorImageInfo, 5> images{};
            std::array<vk::WriteDescriptorSet, 5> writes{};
            for (std::uint32_t i = 0; i < 5; ++i)
            {
                images[i].sampler = texture::sampler();
                images[i].imageView = texture::view(impact::decal_texture_id + i);
                images[i].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                writes[i].dstSet = set;
                writes[i].dstBinding = i + 2;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
                writes[i].pImageInfo = &images[i];
            }
            vk::device::get()->handle.updateDescriptorSets(writes, {});
        }
    }

    void create()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        const auto extent = vk::swapchain::get()->extent;
        const auto count = std::uint32_t(vk::swapchain::get()->images.size());
        if (!count || !extent.width || !extent.height)
            throw std::runtime_error("Invalid blood overlay extent");

        try
        {
            vk::SamplerCreateInfo sampler{};
            sampler.minFilter = vk::Filter::eNearest;
            sampler.magFilter = vk::Filter::eNearest;
            sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
            sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            data.sampler = device.createSampler(sampler);

            std::array<vk::DescriptorSetLayoutBinding, 11> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
                bindings[i].descriptorType = i <= 6 ?
                    vk::DescriptorType::eCombinedImageSampler :
                    i == 7 ? vk::DescriptorType::eStorageImage :
                             vk::DescriptorType::eStorageBuffer;
            }
            vk::DescriptorSetLayoutCreateInfo layout{};
            layout.bindingCount = bindings.size();
            layout.pBindings = bindings.data();
            data.descriptor_layout = device.createDescriptorSetLayout(layout);

            vk::PushConstantRange constant{};
            constant.stageFlags = vk::ShaderStageFlagBits::eCompute;
            constant.size = sizeof(push);
            vk::PipelineLayoutCreateInfo pipeline_layout{};
            pipeline_layout.setLayoutCount = 1;
            pipeline_layout.pSetLayouts = &data.descriptor_layout;
            pipeline_layout.pushConstantRangeCount = 1;
            pipeline_layout.pPushConstantRanges = &constant;
            data.layout = device.createPipelineLayout(pipeline_layout);

            const auto bytecode = vk::shader::read("build/shaders/impact_decal.comp.spv");
            const auto module = vk::shader::create(bytecode);
            try
            {
                vk::ComputePipelineCreateInfo info{};
                info.stage.stage = vk::ShaderStageFlagBits::eCompute;
                info.stage.module = module;
                info.stage.pName = "main";
                info.layout = data.layout;
                data.pipeline = vk::pipeline::compute(info);
            }
            catch (...)
            {
                vk::shader::destroy(module);
                throw;
            }
            vk::shader::destroy(module);

            const std::array<vk::DescriptorPoolSize, 3> pool_sizes{{
                {vk::DescriptorType::eCombinedImageSampler, count * 7},
                {vk::DescriptorType::eStorageImage, count},
                {vk::DescriptorType::eStorageBuffer, count * 3}
            }};
            vk::DescriptorPoolCreateInfo pool{};
            pool.maxSets = count;
            pool.poolSizeCount = pool_sizes.size();
            pool.pPoolSizes = pool_sizes.data();
            data.pool = device.createDescriptorPool(pool);

            data.targets.resize(count);
            const auto columns = (extent.width + tile_size - 1) / tile_size;
            const auto rows = (extent.height + tile_size - 1) / tile_size;
            const auto tiles = std::size_t(columns) * rows;
            const auto memory = vk::MemoryPropertyFlagBits::eHostVisible |
                                vk::MemoryPropertyFlagBits::eHostCoherent;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto& target = data.targets[i];
                vk::ImageCreateInfo image{};
                image.imageType = vk::ImageType::e2D;
                image.format = vk::Format::eR8G8B8A8Unorm;
                image.extent = vk::Extent3D{extent.width, extent.height, 1};
                image.mipLevels = 1;
                image.arrayLayers = 1;
                image.samples = vk::SampleCountFlagBits::e1;
                image.tiling = vk::ImageTiling::eOptimal;
                image.usage = vk::ImageUsageFlagBits::eSampled |
                              vk::ImageUsageFlagBits::eStorage;
                target.overlay = vk::image::create(image, vk::MemoryPropertyFlagBits::eDeviceLocal);

                vk::ImageViewCreateInfo view{};
                view.image = target.overlay.handle;
                view.viewType = vk::ImageViewType::e2D;
                view.format = image.format;
                view.subresourceRange = {
                    vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
                };
                target.overlay_view = vk::image_view::create(view);

                target.decals = vk::buffer::create(
                    max_decals * sizeof(gpu_decal),
                    vk::BufferUsageFlagBits::eStorageBuffer, memory
                );
                target.tile_counts = vk::buffer::create(
                    tiles * sizeof(std::uint32_t),
                    vk::BufferUsageFlagBits::eStorageBuffer, memory
                );
                target.tile_indices = vk::buffer::create(
                    tiles * tile_capacity * sizeof(std::uint32_t),
                    vk::BufferUsageFlagBits::eStorageBuffer, memory
                );

                vk::DescriptorSetAllocateInfo allocate{};
                allocate.descriptorPool = data.pool;
                allocate.descriptorSetCount = 1;
                allocate.pSetLayouts = &data.descriptor_layout;
                target.set = device.allocateDescriptorSets(allocate).front();
                const auto gbuffer = scene_renderer::outputs(i);
                write_target(target, gbuffer.views[3], gbuffer.views[1]);
                bind_atlas(target.set);
            }
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    vk::ImageView overlay_view(std::uint32_t image)
    {
        return get().targets.at(image).overlay_view;
    }

    void record(vk::CommandBuffer command, std::uint32_t image)
    {
        auto& target = get().targets.at(image);
        const auto extent = vk::swapchain::get()->extent;
        const auto vp = scene_renderer::world_view_projection();
        const auto sources = weapon::impact_projectors();

        const auto cols = (extent.width + tile_size - 1) / tile_size;
        const auto rows = (extent.height + tile_size - 1) / tile_size;
        const auto tiles = std::size_t(cols) * rows;
        std::vector<std::uint32_t> counts(tiles, 0);
        std::vector<std::uint32_t> indices(tiles * tile_capacity, 0);
        std::vector<gpu_decal> gpu;
        gpu.reserve(std::min<std::size_t>(sources.size(), max_decals));

        for (const auto& source : sources)
        {
            if (gpu.size() >= max_decals) break;
            if (source.variant >= 5) continue;
            const auto id = std::uint32_t(gpu.size());
            gpu.push_back(convert(source));
            index(source, vp, extent.width, extent.height, counts, indices, id);
        }

        if (!gpu.empty())
            vk::buffer::write(target.decals, gpu.data(), gpu.size() * sizeof(gpu[0]));
        vk::buffer::write(target.tile_counts, counts.data(),
                          counts.size() * sizeof(counts[0]));
        vk::buffer::write(target.tile_indices, indices.data(),
                          indices.size() * sizeof(indices[0]));

        // Streamed textures can be replaced when scene batches arrive.
        bind_atlas(target.set);

        if (!target.initialized)
        {
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eGeneral;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = target.overlay.handle;
            barrier.subresourceRange = {
                vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
            };
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {}, {barrier}
            );
            target.initialized = true;
        }

        const push parameters{
            inverse(vp), extent.width, extent.height,
            std::uint32_t(gpu.size()), .1f
        };
        command.bindPipeline(vk::PipelineBindPoint::eCompute, get().pipeline);
        command.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute, get().layout,
            0, {target.set}, {}
        );
        command.pushConstants(
            get().layout, vk::ShaderStageFlagBits::eCompute,
            0, sizeof(parameters), &parameters
        );
        command.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    }

    void release()
    {
        auto& data = get();
        const auto device = vk::device::get()->handle;
        for (auto& t : data.targets)
        {
            if (t.overlay_view) vk::image_view::destroy(t.overlay_view);
            if (t.overlay.handle) vk::image::destroy(t.overlay);
            if (t.decals.handle) vk::buffer::destroy(t.decals);
            if (t.tile_counts.handle) vk::buffer::destroy(t.tile_counts);
            if (t.tile_indices.handle) vk::buffer::destroy(t.tile_indices);
        }
        data.targets.clear();
        if (data.pipeline) vk::pipeline::destroy(data.pipeline);
        if (data.layout) device.destroyPipelineLayout(data.layout);
        if (data.pool) device.destroyDescriptorPool(data.pool);
        if (data.descriptor_layout) device.destroyDescriptorSetLayout(data.descriptor_layout);
        if (data.sampler) device.destroySampler(data.sampler);
        data.pipeline = nullptr;
        data.layout = nullptr;
        data.pool = nullptr;
        data.descriptor_layout = nullptr;
        data.sampler = nullptr;
    }
} // namespace clt::impact_decal
