#include "blood.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stb_image.h>

namespace clt::blood
{
    namespace
    {
        using namespace vk::util::math;

        constexpr std::uint32_t first_texture = 0x600f0000u;
        constexpr std::array<const char*, 5> decal_names{
            "BloodDecal_0.png", "BloodDecal4.png", "BloodDecal6.png",
            "BloodDecal7.png", "BloodDecal9.png"
        };

        std::uint32_t random(std::uint32_t& seed)
        {
            if (!seed) seed = 1;
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return seed;
        }

        float unit(std::uint32_t& seed)
        {
            return float(random(seed) & 0xffffffu) / float(0x1000000u);
        }

        ::scene::mesh decal_mesh(std::uint32_t id, std::uint32_t variant,
                                 bool textured)
        {
            ::scene::mesh mesh;
            mesh.id = id;
            mesh.surface.base_color = {.32f, .025f, .035f, 1};
            mesh.surface.roughness = .16f;
            mesh.surface.metallic = 0;
            mesh.surface.double_sided = true;
            if (textured)
            {
                mesh.surface.base_color_texture = first_texture + variant;
                mesh.surface.alpha_mode = 1;
                mesh.surface.alpha_cutoff = .05f;
            }

            constexpr std::uint32_t count = 20;
            mesh.vertices.push_back({{0, 0, 0}, {0, 0, 1}, .5f, .5f});
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const float a = float(i) * 6.2831853f / count;
                const float r = 0.46f * (i % 5 == 0 ? .8f : 1.f);
                const auto x = std::cos(a) * r;
                const auto y = std::sin(a) * r;
                mesh.vertices.push_back({
                    {x, y, 0}, {0, 0, 1}, x + .5f, y + .5f
                });
                mesh.indices.insert(mesh.indices.end(),
                    {0u, i + 1, (i + 1) % count + 1});
            }
            return mesh;
        }

        ::scene::mesh droplet_mesh(std::uint32_t id, ::weapon::impact_type type)
        {
            ::scene::mesh mesh;
            mesh.id = id;
            switch (type)
            {
                case ::weapon::impact_type::default_surface:
                    mesh.surface.base_color = {.22f, .21f, .20f, 1};
                    mesh.surface.roughness = 1;
                    break;
                case ::weapon::impact_type::flesh:
                    mesh.surface.base_color = {.22f, .004f, .009f, 1};
                    mesh.surface.roughness = .13f;
                    break;
                case ::weapon::impact_type::metal:
                    mesh.surface.base_color = {.85f, .48f, .07f, 1};
                    mesh.surface.emissive = {6.f, 2.f, .2f};
                    mesh.surface.roughness = .25f;
                    break;
                case ::weapon::impact_type::stone:
                    mesh.surface.base_color = {.32f, .33f, .34f, 1};
                    mesh.surface.roughness = .95f;
                    break;
                case ::weapon::impact_type::wood:
                    mesh.surface.base_color = {.35f, .15f, .055f, 1};
                    mesh.surface.roughness = .88f;
                    break;
                case ::weapon::impact_type::glass:
                    mesh.surface.base_color = {.6f, .78f, .87f, 1};
                    mesh.surface.roughness = .04f;
                    break;
            }
            mesh.surface.metallic = 0;
            mesh.surface.double_sided = true;

            const std::array<vec3, 6> vertices{{
                {0, 1, 0}, {1, 0, 0}, {0, 0, 1},
                {-1, 0, 0}, {0, 0, -1}, {0, -1, 0}
            }};
            constexpr std::array<std::array<std::uint32_t, 3>, 8> faces{{
                {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1},
                {5, 2, 1}, {5, 3, 2}, {5, 4, 3}, {5, 1, 4}
            }};
            for (const auto& face : faces)
            {
                const auto first = std::uint32_t(mesh.vertices.size());
                for (auto index : face)
                {
                    const auto p = vertices[index];
                    mesh.vertices.push_back({p, p, 0, 0});
                }
                mesh.indices.insert(mesh.indices.end(),
                    {first, first + 1, first + 2});
            }
            return mesh;
        }

        void stain_at(system& s, vec3 position, vec3 normal,
                      float size, std::uint32_t& seed)
        {
            if (s.stains.size() >= 2048) s.stains.pop_front();
            s.stains.push_back({
                decal_transform(position, normal, size, unit(seed) * 6.2831853f),
                0, 300, random(seed) % 5u
            });
        }
    }

    mat4 decal_transform(vec3 position, vec3 normal, float size, float angle)
    {
        const auto n = normalize(normal);
        const auto guide = std::abs(n.y) < .95f
            ? vec3{0, 1, 0} : vec3{1, 0, 0};
        auto x = normalize(cross(guide, n));
        auto y = cross(n, x);
        const auto rx = x * std::cos(angle) + y * std::sin(angle);
        const auto ry = y * std::cos(angle) - x * std::sin(angle);
        const auto p = position + n * .002f;

        auto m = mat4::identity();
        for (int i = 0; i < 3; ++i)
        {
            m(i, 0) = (&rx.x)[i] * size;
            m(i, 1) = (&ry.x)[i] * size;
            m(i, 2) = (&n.x)[i] * size;
        }
        m(0, 3) = p.x;
        m(1, 3) = p.y;
        m(2, 3) = p.z;
        return m;
    }

    mat4 drop_transform(vec3 position, float radius)
    {
        auto m = mat4::identity();
        for (int i = 0; i < 3; ++i) m(i, i) = radius;
        m(0, 3) = position.x;
        m(1, 3) = position.y;
        m(2, 3) = position.z;
        return m;
    }

    void append_assets(::scene::state& scene, std::uint32_t first_decal,
                       std::uint32_t drop_mesh_id)
    {
        for (std::uint32_t i = 0; i < decal_names.size(); ++i)
        {
            const auto path = std::filesystem::path("assets/blood/decals") /
                              decal_names[i];
            bool textured = false;
            std::ifstream file(path, std::ios::binary);
            if (file)
            {
                std::vector<std::uint8_t> bytes{
                    std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>()
                };
                int width = 0, height = 0, channels = 0;
                if (!bytes.empty() && bytes.size() <= std::size_t(std::numeric_limits<int>::max()) &&
                    stbi_info_from_memory(bytes.data(), int(bytes.size()),
                                          &width, &height, &channels) &&
                    width > 0 && height > 0)
                {
                    ::scene::texture texture;
                    texture.id = first_texture + i;
                    texture.width = std::uint32_t(width);
                    texture.height = std::uint32_t(height);
                    texture.encoded = std::move(bytes);
                    scene.textures.push_back(std::move(texture));
                    textured = true;
                }
            }
            scene.meshes.push_back(decal_mesh(first_decal + i, i, textured));
        }
        for (std::uint32_t i = 0; i <= std::uint32_t(::weapon::impact_type::glass); ++i)
            scene.meshes.push_back(droplet_mesh(drop_mesh_id + i, ::weapon::impact_type(i)));
    }

    void system::spawn(const net::impact::event& effect)
    {
        std::uint32_t seed = effect.seed ? effect.seed : 1;
        const auto n = normalize(effect.normal);
        const auto through = normalize(effect.direction);
        const auto guide = std::abs(n.y) < .9f
            ? vec3{0, 1, 0} : vec3{1, 0, 0};
        const auto right = normalize(cross(n, guide));
        const auto up = cross(right, n);

        const bool bleeding = effect.type == ::weapon::impact_type::flesh;
        if (bleeding)
        {
            stain_at(*this, effect.position, n, .12f, seed);
            for (int i = 0; i < 4; ++i)
            {
                const auto offset = right * ((unit(seed) - .5f) * .28f) +
                                    up * ((unit(seed) - .5f) * .28f);
                stain_at(*this, effect.position + offset, n,
                    .035f + unit(seed) * .085f, seed);
            }
        }

        const float amount = bleeding ? 140.f :
            effect.type == ::weapon::impact_type::metal ? 32.f :
            effect.type == ::weapon::impact_type::glass ? 48.f : 25.f;
        const auto count = std::uint32_t(std::clamp(
            amount * effect.intensity, 6.f, 180.f));
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const float x = unit(seed) * 2 - 1;
            const float y = unit(seed) * 2 - 1;
            const float speed = (bleeding ? 1.3f : 2.5f) +
                unit(seed) * (bleeding ? 6.5f : 8.0f);
            const auto direction = normalize(
                n * .85f + through * .25f +
                right * x * 1.2f + up * y * 1.2f
            );
            if (drops.size() >= 1024) drops.erase(drops.begin());
            drops.push_back({
                effect.position + n * .035f,
                direction * speed + vec3{0, unit(seed) * 1.5f, 0},
                (bleeding ? .003f : .0015f) +
                    unit(seed) * (bleeding ? .012f : .007f),
                0,
                (bleeding ? .55f : .18f) +
                    unit(seed) * (bleeding ? 1.7f : .8f),
                random(seed),
                effect.type
            });
        }
    }

    void system::update(const ::weapon::world& collision, float dt)
    {
        dt = std::clamp(dt, 0.f, .05f);
        const int steps = std::max(1, int(std::ceil(dt / .008f)));
        const float h = dt / steps;
        for (auto& d : drops)
        {
            if (d.age >= d.lifetime) continue;
            for (int i = 0; i < steps; ++i)
            {
                d.velocity.y -= 9.81f * h;
                const auto delta = d.velocity * h;
                const float length = vk::util::math::length(delta);
                if (length > 1e-7f)
                {
                    const auto hit = collision.trace(d.position, delta / length, length);
                    if (hit.impacted)
                    {
                        if (d.type == ::weapon::impact_type::flesh)
                        {
                            auto seed = d.seed;
                            stain_at(*this, hit.position, hit.normal,
                                .023f + unit(seed) * .1f, seed);
                            if (hit.normal.y > .8f && pools.size() < 64 &&
                                unit(seed) > .75f)
                            {
                                pools.push_back({
                                    hit.position, hit.normal, .035f,
                                    .12f + unit(seed) * .2f, 0,
                                    unit(seed) * 6.2831853f, random(seed) % 5u
                                });
                            }
                        }
                        d.age = d.lifetime;
                        break;
                    }
                }
                d.position = d.position + delta;
            }
            d.age += dt;
        }
        std::erase_if(drops, [](const drop& d)
        {
            return d.age >= d.lifetime || d.position.y < -100;
        });
        for (auto& s : stains) s.age += dt;
        std::erase_if(stains, [](const stain& s) { return s.age >= s.lifetime; });
        for (auto& p : pools)
        {
            p.age += dt;
            const float t = std::clamp(p.age / 2.f, 0.f, 1.f);
            const float smoothed = t * t * (3.f - 2.f * t);
            p.radius = .035f + (p.final_radius - .035f) * smoothed;
        }
    }
} // namespace clt::blood
