#include "texture.hpp"
#define STBI_NO_STDIO
#include <gpu/upload.hpp>
#include <gpu/texture_cache.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <memory>
#include <stb_image.h>
#include <stdexcept>
#include <utility>
#include <vector>

namespace clt::texture
{
    namespace
    {
        struct resource
        {
            std::uint32_t id = 0;
            std::uint32_t hash = 0;
            vk::ImageView view{};
        };

        struct state
        {
            vk::Sampler sampler{};
            resource fallback;
            std::vector<resource> textures;
            gpu::texture_cache cache;
        };

        state& get()
        {
            static state data;
            return data;
        }

        std::uint32_t checksum(const std::vector<std::uint8_t>& bytes)
        {
            std::uint32_t hash = 2166136261u;
            for (auto value : bytes) hash = (hash ^ value) * 16777619u;
            return hash;
        }

        void destroy(resource& texture)
        {
            texture = {};
        }

        resource create(const ::scene::texture& source, gpu::upload::batch& batch)
        {
            resource result;
            result.id = source.id;
            result.hash = checksum(source.encoded.empty() ? source.pixels : source.encoded);
            try
            {
                // Locally imported models retain compressed images just like the
                // server. Streamed images can already contain decoded pixels.
                std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
                    nullptr, stbi_image_free
                );
                const auto* pixels = source.pixels.data();
                if (source.pixels.empty())
                {
                    int width = 0, height = 0, channels = 0;
                    if (source.encoded.empty() || source.encoded.size() > INT_MAX)
                    {
                        throw std::runtime_error("Missing local texture data");
                    }
                    decoded.reset(stbi_load_from_memory(
                        source.encoded.data(),
                        int(source.encoded.size()),
                        &width,
                        &height,
                        &channels,
                        4
                    ));
                    if (!decoded || width != int(source.width) || height != int(source.height))
                    {
                        throw std::runtime_error("Invalid local texture dimensions");
                    }
                    pixels = decoded.get();
                }
                else if (source.pixels.size() != std::size_t(source.width) * source.height * 4)
                {
                    throw std::runtime_error("Invalid texture pixel count");
                }
                get().cache.upsert(
                    result.id, result.hash, pixels, source.width, source.height, batch
                );
                result.view = get().cache.view(result.id);
            }
            catch (...)
            {
                destroy(result);
                throw;
            }
            return result;
        }

        const resource& find(std::uint32_t id)
        {
            for (const auto& texture : get().textures)
                if (texture.id == id) return texture;
            return get().fallback;
        }
    } // namespace

    void init()
    {
        auto& data = get();
        if (data.sampler) throw std::runtime_error("Texture system already initialized");
        vk::SamplerCreateInfo info{};
        info.magFilter = vk::Filter::eLinear;
        info.minFilter = vk::Filter::eLinear;
        info.mipmapMode = vk::SamplerMipmapMode::eLinear;
        info.addressModeU = vk::SamplerAddressMode::eRepeat;
        info.addressModeV = vk::SamplerAddressMode::eRepeat;
        info.addressModeW = vk::SamplerAddressMode::eRepeat;
        data.sampler = vk::device::get()->handle.createSampler(info);

        try
        {
            gpu::upload::batch batch;
            ::scene::texture white;
            white.id = 0;
            white.width = white.height = 1;
            white.pixels = {255, 255, 255, 255};
            data.fallback = create(white, batch);
            batch.finish();
        }
        catch (...)
        {
            shutdown();
            throw;
        }
    }

    namespace
    {
        std::vector<::scene::texture> pinned;
    }

    void pin(const ::scene::state& source)
    {
        pinned = source.textures;
    }

    void synchronize(const ::scene::state& source)
    {
        auto& data = get();
        gpu::upload::batch batch;
        std::vector<resource> replacement;
        replacement.reserve(source.textures.size() + pinned.size());
        std::vector<const ::scene::texture*> incoming;
        for (const auto& t : source.textures)
        {
            incoming.push_back(&t);
        }
        for (const auto& t : pinned)
        {
            incoming.push_back(&t);
        }
        try
        {
            for (const auto* entry : incoming)
            {
                const auto& texture = *entry;
                if (std::any_of(
                        replacement.begin(),
                        replacement.end(),
                        [&](const resource& item)
                        {
                            return item.id == texture.id;
                        }
                    ))
                {
                    continue;
                }
                const auto hash =
                    checksum(texture.encoded.empty() ? texture.pixels : texture.encoded);
                bool reused = false;
                for (auto& old : data.textures)
                {
                    if (old.id == texture.id && old.hash == hash)
                    {
                        replacement.push_back(std::exchange(old, resource{}));
                        reused = true;
                        break;
                    }
                }
                if (!reused) replacement.push_back(create(texture, batch));
            }
            batch.finish();
        }
        catch (...)
        {
            for (auto& resource : replacement) destroy(resource);
            throw;
        }
        for (auto& old : data.textures) destroy(old);
        data.textures = std::move(replacement);
        std::vector<std::uint64_t> keep{0};
        for (const auto& entry : data.textures) keep.push_back(entry.id);
        data.cache.retain(keep);
    }

    void bind(vk::DescriptorSet set, const ::scene::material& surface)
    {
        const std::array<std::uint32_t, 5> ids{
            surface.base_color_texture,
            surface.metallic_roughness_texture,
            surface.normal_texture,
            surface.occlusion_texture,
            surface.emissive_texture
        };
        std::array<vk::DescriptorImageInfo, 5> images{};
        std::array<vk::WriteDescriptorSet, 5> writes{};
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            images[i].sampler = get().sampler;
            images[i].imageView = find(ids[i]).view;
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
        for (auto& resource : get().textures) destroy(resource);
        get().textures.clear();
        get().cache.retain({0});
    }

    void shutdown()
    {
        reset();
        pinned.clear();
        auto& data = get();
        destroy(data.fallback);
        data.cache.clear();
        if (data.sampler) vk::device::get()->handle.destroySampler(data.sampler);
        data.sampler = nullptr;
    }
} // namespace clt::texture
