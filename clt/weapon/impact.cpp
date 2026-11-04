#include "impact.hpp"

#include "clt/network/connection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stb_image.h>

namespace clt::impact
{
    namespace
    {
        using namespace vk::util::math;

        constexpr std::array<const char*, 5> decal_names{
            "BloodDecal_0.png", "BloodDecal4.png", "BloodDecal6.png",
            "BloodDecal7.png", "BloodDecal9.png"
        };
        constexpr float pi = 3.14159265f;

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

        mat4 inverse_affine(const mat4& m)
        {
            const vec3 a{m(0, 0), m(1, 0), m(2, 0)};
            const vec3 b{m(0, 1), m(1, 1), m(2, 1)};
            const vec3 c{m(0, 2), m(1, 2), m(2, 2)};
            const float det = dot(a, cross(b, c));
            if (std::abs(det) < 1e-9f) return mat4::identity();
            const vec3 x = cross(b, c) / det;
            const vec3 y = cross(c, a) / det;
            const vec3 z = cross(a, b) / det;
            mat4 out = mat4::identity();
            for (int i = 0; i < 3; ++i)
            {
                out(0, i) = (&x.x)[i];
                out(1, i) = (&y.x)[i];
                out(2, i) = (&z.x)[i];
            }
            const vec3 p{m(0, 3), m(1, 3), m(2, 3)};
            out(0, 3) = -dot(x, p);
            out(1, 3) = -dot(y, p);
            out(2, 3) = -dot(z, p);
            return out;
        }

        mat4 instance_matrix(const ::scene::state* scene, std::uint32_t id)
        {
            if (!scene || !id) return mat4::identity();
            const auto found = std::find_if(scene->instances.begin(), scene->instances.end(),
                [&](const ::scene::instance& item) { return item.id == id; });
            if (found == scene->instances.end()) return mat4::identity();
            return trs(found->pose.position, found->pose.rotation, found->pose.scale);
        }

        bool owns_instance(const ::scene::state* scene, std::uint32_t id)
        {
            if (!scene || !id) return false;
            return std::any_of(scene->instances.begin(), scene->instances.end(),
                [&](const ::scene::instance& item) { return item.id == id; });
        }

        void stain_at(system& s, vec3 position, vec3 normal, float size,
                      std::uint32_t& seed, std::uint32_t target = 0,
                      float stretch = 1.f)
        {
            if (s.stains.size() >= 768) s.stains.pop_front();
            stain item;
            item.variant = 1u + random(seed) % 4u;
            item.wetness = .82f + unit(seed) * .18f;
            item.lifetime = 280;
            item.transform = decal_transform(position, normal, size, unit(seed) * 2.f * pi);
            for (int r = 0; r < 3; ++r) item.transform(r, 1) *= stretch;
            const auto scene = connection::snapshot();
            if (owns_instance(scene.get(), target))
            {
                item.anchor = target;
                item.local = inverse_affine(instance_matrix(scene.get(), target)) *
                             item.transform;
            }
            s.stains.push_back(item);
        }

        ::scene::mesh droplet_mesh(std::uint32_t id)
        {
            ::scene::mesh mesh;
            mesh.id = id;
            mesh.surface.base_color = {.16f, .004f, .009f, 1};
            mesh.surface.roughness = .13f;
            mesh.surface.metallic = 0;
            mesh.surface.double_sided = true;

            constexpr std::array<vec3, 6> vertices{{
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
                for (const auto index : face)
                {
                    const auto p = vertices[index];
                    mesh.vertices.push_back({p, p, 0, 0});
                }
                mesh.indices.insert(mesh.indices.end(),
                    {first, first + 1, first + 2});
            }
            return mesh;
        }

        void push_drop(system& s, vec3 position, vec3 velocity,
                       float radius, float lifetime, std::uint32_t seed)
        {
            if (s.drops.size() >= 128) s.drops.erase(s.drops.begin());
            s.drops.push_back({position, velocity, radius, 0, lifetime, seed});
        }
    }

    mat4 decal_transform(vec3 position, vec3 normal, float size, float angle)
    {
        const auto n = normalize(normal);
        const auto guide = std::abs(n.y) < .95f ?
            vec3{0, 1, 0} : vec3{1, 0, 0};
        const auto x = normalize(cross(guide, n));
        const auto y = cross(n, x);
        const auto rx = x * std::cos(angle) + y * std::sin(angle);
        const auto ry = y * std::cos(angle) - x * std::sin(angle);

        auto m = mat4::identity();
        for (int i = 0; i < 3; ++i)
        {
            m(i, 0) = (&rx.x)[i] * size;
            m(i, 1) = (&ry.x)[i] * size;
            // Keep a real projection volume, even for narrow splatter marks.
            m(i, 2) = (&n.x)[i] * std::max(.15f, size);
        }
        m(0, 3) = position.x + n.x * .002f;
        m(1, 3) = position.y + n.y * .002f;
        m(2, 3) = position.z + n.z * .002f;
        return m;
    }

    mat4 drop_transform(vec3 position, float radius, vec3 velocity)
    {
        const auto length = vk::util::math::length(velocity);
        const vec3 up = length > .0001f ? velocity / length : vec3{0, 1, 0};
        const vec3 helper = std::abs(up.y) < .95f ? vec3{0, 1, 0} : vec3{1, 0, 0};
        const vec3 side = normalize(cross(helper, up));
        const vec3 normal = cross(up, side);
        const float streak = radius * (1.2f + std::min(length * .55f, 4.f));
        auto m = mat4::identity();
        for (int i = 0; i < 3; ++i)
        {
            m(i, 0) = (&side.x)[i] * radius;
            m(i, 1) = (&up.x)[i] * streak;
            m(i, 2) = (&normal.x)[i] * radius;
        }
        m(0, 3) = position.x;
        m(1, 3) = position.y;
        m(2, 3) = position.z;
        return m;
    }

    void append_assets(::scene::state& scene, std::uint32_t first_decal,
                       std::uint32_t drop_mesh_id)
    {
        (void)first_decal;
        for (std::uint32_t i = 0; i < decal_names.size(); ++i)
        {
            const auto path = std::filesystem::path("assets/blood/decals") /
                              decal_names[i];
            std::ifstream file(path, std::ios::binary);
            if (!file) continue;

            std::vector<std::uint8_t> bytes{
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()
            };
            int width = 0, height = 0, channels = 0;
            if (bytes.empty() ||
                bytes.size() > std::size_t(std::numeric_limits<int>::max()) ||
                !stbi_info_from_memory(bytes.data(), int(bytes.size()),
                                       &width, &height, &channels) ||
                width <= 0 || height <= 0)
                continue;

            ::scene::texture texture;
            texture.id = decal_texture_id + i;
            texture.width = std::uint32_t(width);
            texture.height = std::uint32_t(height);
            texture.encoded = std::move(bytes);
            scene.textures.push_back(std::move(texture));
        }
        // There is only ONE effect mesh: blood. No metal/wood/stone/glass particles.
        scene.meshes.push_back(droplet_mesh(drop_mesh_id));
    }

    void system::spawn(const net::impact::event& effect)
    {
        if (effect.type != ::weapon::impact_type::flesh) return;

        std::uint32_t seed = effect.seed ? effect.seed : 1;
        const auto normal = normalize(effect.normal);
        const auto direction = normalize(effect.direction);
        if (vk::util::math::length(direction) < .001f) return;

        const auto helper = std::abs(direction.y) < .90f ?
            vec3{0, 1, 0} : vec3{1, 0, 0};
        const auto right = normalize(cross(direction, helper));
        const auto up = cross(right, direction);

        if (effect.uv_valid)
        {
            if (wounds.size() >= 256) wounds.pop_front();
            wound item;
            item.target = effect.target;
            item.uv = effect.uv;
            item.radius = .024f + unit(seed) * .023f;
            item.strength = .82f + unit(seed) * .18f;
            item.angle = unit(seed) * pi * 2.f;
            item.position = effect.position;
            item.seed = random(seed);
            item.next_drip = 1.1f + unit(seed) * .7f;
            const auto scene = connection::snapshot();
            item.local = owns_instance(scene.get(), item.target)
                ? inverse_affine(instance_matrix(scene.get(), item.target)) *
                    vec4{item.position, 1.f}
                : vec4{item.position, 1.f};
            wounds.push_back(item);
        }

        // A real hit yields a coherent direction, not an isotropic cloud of 140 dots.
        const auto count = std::uint32_t(std::clamp(
            7.f + effect.intensity * 3.f, 6.f, 14.f));
        const auto origin = effect.position + direction * .04f;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const float radial = std::sqrt(unit(seed)) * (.14f + .14f * effect.intensity);
            const float phase = unit(seed) * pi * 2.f;
            const auto eject = normalize(
                direction + right * (std::cos(phase) * radial) +
                up * (std::sin(phase) * radial) + normal * .09f);
            const auto speed = (2.6f + unit(seed) * 4.6f) *
                               std::clamp(effect.intensity, .45f, 1.6f);
            push_drop(*this, origin, eject * speed,
                .012f + unit(seed) * .014f, 1.2f + unit(seed) * 1.1f,
                random(seed));
        }

        if (pending_backdrops.size() >= 32) pending_backdrops.erase(pending_backdrops.begin());
        pending_backdrops.push_back({effect.position, direction,
                                     effect.intensity, random(seed), effect.target});
    }

    std::vector<projector> system::projectors(const ::scene::state* scene) const
    {
        std::vector<projector> result;
        result.reserve(stains.size() + pools.size());
        for (const auto& item : stains)
        {
            const auto m = item.anchor && owns_instance(scene, item.anchor)
                ? instance_matrix(scene, item.anchor) * item.local
                : item.transform;
            const auto age = std::clamp(item.age / item.lifetime, 0.f, 1.f);
            result.push_back({
                m, item.variant, std::clamp(item.wetness * (1.f - age), .2f, 1.f)
            });
        }
        for (const auto& item : pools)
        {
            const auto basis = item.anchor && owns_instance(scene, item.anchor)
                ? instance_matrix(scene, item.anchor) * item.local
                : decal_transform(item.position, item.normal, 1, item.angle);
            const auto m = basis * scale({item.radius, item.radius, 1.f});
            const auto age = std::clamp(item.age / item.lifetime, 0.f, 1.f);
            result.push_back({
                m, item.variant, std::clamp(1.f - age, .2f, 1.f)
            });
        }
        return result;
    }

    void system::update(const ::weapon::world& collision, float dt)
    {
        dt = std::clamp(dt, 0.f, .05f);
        const auto scene = connection::snapshot();

        for (const auto& b : pending_backdrops)
        {
            // Skip the struck actor's geometry: only stain a real receiving surface.
            auto start = b.position + b.direction * .065f;
            for (int attempts = 0; attempts < 6; ++attempts)
            {
                const auto hit = collision.trace(start, b.direction, 4.f);
                if (!hit.impacted) break;
                if (hit.instance != b.target)
                {
                    auto seed = b.seed;
                    stain_at(*this, hit.position, hit.normal,
                        .35f + unit(seed) * .23f, seed, hit.instance, 1.5f);
                    break;
                }
                start = hit.position + b.direction * .04f;
            }
        }
        pending_backdrops.clear();

        const int steps = std::max(1, int(std::ceil(dt / .012f)));
        const float h = dt / float(steps);
        for (auto& d : drops)
        {
            if (d.age >= d.lifetime) continue;
            for (int i = 0; i < steps; ++i)
            {
                d.velocity = d.velocity * (1.f - .17f * h);
                d.velocity.y -= 9.81f * h;
                const auto delta = d.velocity * h;
                const float distance = vk::util::math::length(delta);
                if (distance > 1e-7f)
                {
                    const auto hit = collision.trace(
                        d.position, delta / distance, distance);
                    if (hit.impacted)
                    {
                        auto seed = d.seed;
                        stain_at(*this, hit.position, hit.normal,
                            .13f + unit(seed) * .14f, seed, hit.instance,
                            1.1f + unit(seed) * 1.6f);
                        if (hit.normal.y > .72f && pools.size() < 32 &&
                            unit(seed) > .55f)
                        {
                            pool p;
                            p.position = hit.position;
                            p.normal = hit.normal;
                            p.radius = .07f;
                            p.final_radius = .25f + unit(seed) * .23f;
                            p.angle = unit(seed) * pi * 2.f;
                            p.variant = 1u + random(seed) % 4u;
                            p.anchor = hit.instance;
                            if (owns_instance(scene.get(), p.anchor))
                                p.local = inverse_affine(instance_matrix(scene.get(), p.anchor)) *
                                    decal_transform(p.position, p.normal, 1.f, p.angle);
                            pools.push_back(p);
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
            return d.age >= d.lifetime || d.position.y < -100.f;
        });

        for (auto& w : wounds)
        {
            w.age += dt;
            if (w.age > 7.f || w.age < w.next_drip) continue;
            auto seed = w.seed;
            const auto location = owns_instance(scene.get(), w.target)
                ? instance_matrix(scene.get(), w.target) * w.local
                : w.local;
            const vec3 position{location.x, location.y, location.z};
            push_drop(*this, position,
                vec3{(unit(seed) - .5f) * .3f, -.2f,
                     (unit(seed) - .5f) * .3f},
                .012f, 2.2f, random(seed));
            w.seed = seed;
            w.next_drip += 1.0f + unit(seed) * 1.5f;
        }

        for (auto& s : stains) s.age += dt;
        std::erase_if(stains, [](const stain& s) {
            return s.age >= s.lifetime;
        });
        for (auto& p : pools)
        {
            p.age += dt;
            const float t = std::clamp(p.age / 3.5f, 0.f, 1.f);
            p.radius = .07f + (p.final_radius - .07f) *
                (t * t * (3.f - 2.f * t));
        }
        std::erase_if(pools, [](const pool& p) {
            return p.age >= p.lifetime;
        });
    }
} // namespace clt::impact
