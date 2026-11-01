#include "image.hpp"
#include "srv/shared/net/scene_stream.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#include <fastgltf/tools.hpp>

#include <climits>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>

namespace srv::assets
{
    namespace
    {
        std::span<const std::byte> encoded(const fastgltf::Asset& asset,
                                            const fastgltf::Image& image)
        {
            if (const auto* array = std::get_if<fastgltf::sources::Array>(&image.data))
                return {array->bytes.data(), array->bytes.size()};

            if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&image.data))
            {
                const auto& region = asset.bufferViews.at(view->bufferViewIndex);
                const auto& buffer = asset.buffers.at(region.bufferIndex);
                if (const auto* array = std::get_if<fastgltf::sources::Array>(&buffer.data))
                {
                    if (region.byteOffset > array->bytes.size() ||
                        region.byteLength > array->bytes.size() - region.byteOffset)
                        throw std::runtime_error("Invalid glTF image buffer view");
                    return {array->bytes.data() + region.byteOffset, region.byteLength};
                }
            }
            throw std::runtime_error("Unsupported glTF image source");
        }
    } // namespace

    std::vector<::scene::texture> images(const fastgltf::Asset& asset,
                                          std::uint32_t first_texture)
    {
        std::vector<::scene::texture> result;
        result.reserve(asset.images.size());
        for (std::size_t i = 0; i < asset.images.size(); ++i)
        {
            const auto source = encoded(asset, asset.images[i]);
            if (source.empty() || source.size() > INT_MAX)
                throw std::runtime_error("Invalid glTF image size");
            int w = 0, h = 0, channels = 0;
            auto* pixels = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(source.data()),
                int(source.size()), &w, &h, &channels, 4);
            if (!pixels || w <= 0 || h <= 0 ||
                std::uint64_t(w) * h * 4 > net::scene_stream::max_scene_bytes - 128)
            {
                stbi_image_free(pixels);
                throw std::runtime_error("Invalid or oversized glTF texture");
            }

            ::scene::texture texture;
            texture.id = first_texture + std::uint32_t(i);
            texture.width = std::uint32_t(w);
            texture.height = std::uint32_t(h);
            texture.pixels.assign(pixels, pixels + std::size_t(w) * h * 4);
            stbi_image_free(pixels);
            result.push_back(std::move(texture));
        }
        return result;
    }
} // namespace srv::assets
