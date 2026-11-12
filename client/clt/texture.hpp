#pragma once
#include "../../server/scene.hpp"
#include <climits>
#include <gpu/texture_cache.hpp>
#include <gpu/upload.hpp>
#include <memory>
#include <stb_image.h>
#include <unordered_set>
#include <vk/vk.hpp>

namespace clt::texture
{
void synchronize(const ::scene::state &source);
void bind(vk::DescriptorSet set, const ::scene::material &surface);
void pin(const ::scene::state &source);
void reset();
void shutdown();

namespace
{
gpu::texture_cache cache;
vk::Sampler sampler{};
const ::scene::state *pinned = nullptr;
std::unordered_set<std::uint32_t> live;
void upload(const ::scene::texture &texture, gpu::upload::batch &batch)
{
    const auto &bytes = texture.encoded.empty() ? texture.pixels : texture.encoded;

    const auto hash = net::scene_stream::checksum(bytes);

    if (cache.contains(texture.id, hash)) return;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(nullptr, stbi_image_free);

    const stbi_uc *pixels = texture.pixels.data();

    if (texture.pixels.empty()) {
        int width = 0, height = 0, channels = 0;

        if (texture.encoded.empty() || texture.encoded.size() > INT_MAX)
            throw std::runtime_error("Missing local texture data");
        decoded.reset(stbi_load_from_memory(texture.encoded.data(), int(texture.encoded.size()), &width, &height, &channels, 4));

        if (!decoded || width != int(texture.width) || height != int(texture.height))
            throw std::runtime_error("Invalid local texture dimensions");
        pixels = decoded.get();
    }
    else if (!texture.width || !texture.height || texture.pixels.size() != std::size_t(texture.width) * texture.height * 4u)
        throw std::runtime_error("Invalid texture pixel count");
    cache.upsert(texture.id, hash, pixels, texture.width, texture.height, batch);
}
} // namespace
void init()
{
    if (sampler) throw std::runtime_error("Texture system already initialized");
    vk::SamplerCreateInfo info{};
    info.magFilter = info.minFilter = vk::Filter::eLinear;
    info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    info.addressModeU = info.addressModeV = info.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler = vk::device::get()->handle.createSampler(info);
    try
    {
        gpu::upload::batch batch;
        ::scene::texture white{};
        white.id = 0;
        white.width = white.height = 1;
        white.pixels = {255, 255, 255, 255};
        upload(white, batch);
        batch.finish();
    }

    catch (...)
    {
        shutdown();
        throw;
    }
}

void pin(const ::scene::state &scene) { pinned = &scene; }
void synchronize(const ::scene::state &scene)
{
    gpu::upload::batch batch;
    std::unordered_set<std::uint32_t> updated;
    updated.reserve(scene.textures.size() + (pinned ? pinned->textures.size() : 0));
    std::vector<std::uint64_t> keep{0};
    keep.reserve(scene.textures.size() + (pinned ? pinned->textures.size() : 0) + 1);

    const auto process = [&](const ::scene::texture &texture)
    {
        if (!updated.insert(texture.id).second) return;
        upload(texture, batch);
        keep.push_back(texture.id);
    };

    for (const auto &texture : scene.textures)
        process(texture);

    if (pinned)
        for (const auto &texture : pinned->textures)
            process(texture);
    batch.finish();
    cache.retain(keep);
    live = std::move(updated);
}

void bind(vk::DescriptorSet set, const ::scene::material &material)
{
    const std::array<std::uint32_t, 5> ids{
        material.base_color_texture, material.metallic_roughness_texture, material.normal_texture, material.occlusion_texture, material.emissive_texture
    };

    std::array<vk::DescriptorImageInfo, 5> images{};
    std::array<vk::WriteDescriptorSet, 5> writes{};

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        images[i].sampler = sampler;
        images[i].imageView = cache.view(live.contains(ids[i]) ? ids[i] : 0u);
        images[i].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        writes[i].dstSet = set;
        writes[i].dstBinding = std::uint32_t(4 + i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[i].pImageInfo = &images[i];
    }

    vk::device::get()->handle.updateDescriptorSets(writes, {});
}

void reset()
{
    live.clear();
    cache.retain({0});
}

void shutdown()
{
    reset();
    pinned = nullptr;
    cache.clear();

    if (sampler) vk::device::get()->handle.destroySampler(sampler);
    sampler = nullptr;
}

} // namespace clt::texture
