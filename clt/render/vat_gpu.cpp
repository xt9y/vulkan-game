#include "vat_gpu.hpp"

#include "upload.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <stb_image.h>

namespace clt::vat_gpu
{
    namespace
    {
        constexpr std::array<std::uint32_t, 3> types{19, 20, 22};

        struct resource
        {
            std::uint32_t type = 0;
            float fps = 24;
            std::uint32_t frames = 60;
            std::array<float, 3> low{}, high{};
            std::array<vk::image::resource, 3> images{};
            std::array<vk::ImageView, 3> views{};
        };

        struct state
        {
            vk::Sampler sampler{};
            std::array<resource, 3> resources{};
        };

        state& get()
        {
            static state data;
            return data;
        }

        std::filesystem::path folder(std::uint32_t type)
        {
            return std::filesystem::path("assets/blood/vat") /
                   ("Blood" + std::to_string(type));
        }

        std::vector<std::uint8_t> read(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) throw std::runtime_error("Cannot read VAT: " + path.string());
            const auto size = file.tellg();
            if (size <= 0 || size > std::streamoff(512ull * 1024 * 1024))
                throw std::runtime_error("Invalid VAT file size: " + path.string());
            std::vector<std::uint8_t> result(std::size_t(size), std::uint8_t{});
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(result.data()), size))
                throw std::runtime_error("VAT read failed: " + path.string());
            return result;
        }

        float number(const std::string& text, const char* key)
        {
            const auto found = text.find(std::string("\"") + key + "\"");
            if (found == std::string::npos)
                throw std::runtime_error("Missing VAT metadata field: " + std::string(key));
            const auto colon = text.find(':', found);
            if (colon == std::string::npos)
                throw std::runtime_error("Malformed VAT metadata");
            const float result = std::stof(text.substr(colon + 1));
            if (!std::isfinite(result))
                throw std::runtime_error("Nonfinite VAT metadata");
            return result;
        }

        std::array<float, 3> array3(const std::string& text, const char* key)
        {
            const auto found = text.find(std::string("\"") + key + "\"");
            if (found == std::string::npos)
                throw std::runtime_error("Missing VAT bounds");
            const auto begin = text.find('[', found);
            const auto end = text.find(']', begin);
            if (begin == std::string::npos || end == std::string::npos)
                throw std::runtime_error("Malformed VAT bounds");
            auto values = text.substr(begin + 1, end - begin - 1);
            std::replace(values.begin(), values.end(), ',', ' ');
            std::istringstream stream(values);
            std::array<float, 3> result{};
            for (auto& value : result)
                if (!(stream >> value) || !std::isfinite(value))
                    throw std::runtime_error("Malformed VAT bounds value");
            return result;
        }

        void metadata(resource& item)
        {
            const auto bytes = read(folder(item.type) / ("Blood" + std::to_string(item.type) + ".json"));
            const std::string text(bytes.begin(), bytes.end());
            item.fps = number(text, "fps");
            item.frames = std::uint32_t(number(text, "frameCount"));
            item.low = array3(text, "boundsMin");
            item.high = array3(text, "boundsMax");
            if (item.fps <= 0 || !item.frames || item.frames > 4096)
                throw std::runtime_error("Invalid VAT animation timing");
        }

        struct pixels
        {
            std::uint32_t width = 0, height = 0;
            std::vector<std::uint8_t> data;
        };

        pixels half_texture(std::uint32_t type, const char* channel)
        {
            const auto path = folder(type) /
                ("Blood" + std::to_string(type) + "_" + channel + ".rgba16f");
            const auto raw = read(path);
            if (raw.size() < 12 || std::memcmp(raw.data(), "VTX1", 4) != 0)
                throw std::runtime_error("Invalid VAT cache header: " + path.string());
            pixels output;
            std::memcpy(&output.width, raw.data() + 4, 4);
            std::memcpy(&output.height, raw.data() + 8, 4);
            if (!output.width || !output.height ||
                output.width > 8192 || output.height > 8192 ||
                std::uint64_t(output.width) * output.height * 8 != raw.size() - 12)
                throw std::runtime_error("Invalid VAT cache dimensions");
            output.data.assign(raw.begin() + 12, raw.end());
            return output;
        }

        pixels lookup_texture(std::uint32_t type)
        {
            const auto path = folder(type) /
                ("Blood" + std::to_string(type) + "_lookup.png");
            const auto bytes = read(path);
            int width = 0, height = 0, channels = 0;
            if (bytes.size() > std::size_t(std::numeric_limits<int>::max()))
                throw std::runtime_error("VAT lookup too large");
            auto* decoded = stbi_load_from_memory(bytes.data(), int(bytes.size()),
                &width, &height, &channels, 4);
            if (!decoded || width <= 0 || height <= 0 ||
                width > 8192 || height > 8192)
            {
                stbi_image_free(decoded);
                throw std::runtime_error("Invalid VAT lookup: " + path.string());
            }
            pixels result;
            result.width = std::uint32_t(width);
            result.height = std::uint32_t(height);
            result.data.assign(decoded, decoded + std::size_t(width) * height * 4);
            stbi_image_free(decoded);
            return result;
        }

        const resource* find(std::uint32_t type)
        {
            for (const auto& item : get().resources)
                if (item.type == type) return &item;
            return nullptr;
        }
    }

    bool prepared(std::uint32_t type)
    {
        if (std::find(types.begin(), types.end(), type) == types.end())
            return false;
        const auto prefix = "Blood" + std::to_string(type);
        const auto path = folder(type);
        return std::filesystem::is_regular_file("assets/blood/blood" + std::to_string(type) + ".glb") &&
            std::filesystem::is_regular_file(path / (prefix + ".json")) &&
            std::filesystem::is_regular_file(path / (prefix + "_pos.rgba16f")) &&
            std::filesystem::is_regular_file(path / (prefix + "_rot.rgba16f")) &&
            std::filesystem::is_regular_file(path / (prefix + "_lookup.png"));
    }

    void init()
    {
        auto& data = get();
        if (data.sampler)
            throw std::runtime_error("VAT GPU resources already initialized");

        const auto device = vk::device::get()->handle;
        vk::SamplerCreateInfo sampler{};
        sampler.magFilter = vk::Filter::eNearest;
        sampler.minFilter = vk::Filter::eNearest;
        sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        data.sampler = device.createSampler(sampler);

        try
        {
            upload::batch uploads;
            for (std::size_t i = 0; i < types.size(); ++i)
            {
                if (!prepared(types[i])) continue;
                auto& item = data.resources[i];
                item.type = types[i];
                metadata(item);

                const std::array<pixels, 3> sources{
                    half_texture(item.type, "pos"),
                    half_texture(item.type, "rot"),
                    lookup_texture(item.type)
                };
                for (std::size_t j = 0; j < sources.size(); ++j)
                {
                    item.images[j] = uploads.add_image(
                        sources[j].data.data(), sources[j].width, sources[j].height,
                        j == 2 ? vk::Format::eR8G8B8A8Unorm :
                                 vk::Format::eR16G16B16A16Sfloat
                    );
                    vk::ImageViewCreateInfo view{};
                    view.image = item.images[j].handle;
                    view.viewType = vk::ImageViewType::e2D;
                    view.format = item.images[j].format;
                    view.subresourceRange = {
                        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
                    };
                    item.views[j] = vk::image_view::create(view);
                }
                std::cout << "clt: VAT Blood" << item.type << " ready ("
                          << item.frames << " frames at " << item.fps << " FPS)\n";
            }
            uploads.finish();
        }
        catch (...)
        {
            shutdown();
            throw;
        }
    }

    void bind(std::uint32_t type, vk::DescriptorSet set)
    {
        const auto* item = find(type);
        if (!item || !item->images[0].handle) return;
        std::array<vk::DescriptorImageInfo, 3> infos{};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            infos[i].sampler = get().sampler;
            infos[i].imageView = item->views[i];
            infos[i].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            writes[i].dstSet = set;
            writes[i].dstBinding = 10 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[i].pImageInfo = &infos[i];
        }
        vk::device::get()->handle.updateDescriptorSets(writes, {});
    }

    parameters_t parameters(std::uint32_t type, float age, bool mirror)
    {
        parameters_t result;
        const auto* item = find(type);
        if (!item) return result;
        result.timing = {age, item->fps, float(item->frames), mirror ? 1.f : 0.f};
        result.bounds_min = {item->low[0], item->low[1], item->low[2], 0};
        result.bounds_max = {item->high[0], item->high[1], item->high[2], 0};
        return result;
    }

    void shutdown() noexcept
    {
        auto& data = get();
        for (auto& item : data.resources)
        {
            for (std::size_t i = 0; i < item.images.size(); ++i)
            {
                if (item.views[i]) vk::image_view::destroy(item.views[i]);
                if (item.images[i].handle) vk::image::destroy(item.images[i]);
            }
            item = {};
        }
        if (data.sampler)
            vk::device::get()->handle.destroySampler(data.sampler);
        data.sampler = nullptr;
    }
} // namespace clt::vat_gpu
