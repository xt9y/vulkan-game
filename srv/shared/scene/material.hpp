#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace scene
{
    // glTF metallic-roughness parameters. Texture references are imported by
    // the asset loader, independent of the authoritative scene simulation.
    struct texture
    {
        std::uint32_t id = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;
    };

    struct material
    {
        std::array<float, 4> base_color{1, 1, 1, 1};
        std::array<float, 3> emissive{0, 0, 0};
        float metallic = 0;
        float roughness = 1;
        float occlusion = 1;
        float alpha_cutoff = 0.5f;
        std::uint32_t base_color_texture = 0;
        std::uint32_t metallic_roughness_texture = 0;
        std::uint32_t normal_texture = 0;
        std::uint32_t occlusion_texture = 0;
        std::uint32_t emissive_texture = 0;
        std::uint32_t alpha_mode = 0; // opaque, mask, blend
        bool double_sided = false;
    };

    inline bool same_material(const material& a, const material& b)
    {
        return a.base_color == b.base_color &&
            a.emissive == b.emissive &&
            a.metallic == b.metallic &&
            a.roughness == b.roughness &&
            a.occlusion == b.occlusion &&
            a.alpha_cutoff == b.alpha_cutoff &&
            a.base_color_texture == b.base_color_texture &&
            a.metallic_roughness_texture == b.metallic_roughness_texture &&
            a.normal_texture == b.normal_texture &&
            a.occlusion_texture == b.occlusion_texture &&
            a.emissive_texture == b.emissive_texture &&
            a.alpha_mode == b.alpha_mode &&
            a.double_sided == b.double_sided;
    }
} // namespace scene
