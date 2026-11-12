#pragma once
#include "movement/detail.hpp"
#include "../scene.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <stb_image.h>
#include <stdexcept>
#include <utility>
#include <vector>

namespace net::scene_stream
{
inline constexpr std::size_t max_scene_bytes = 75 * 1024 * 1024;
struct batch_payload
{
    std::uint32_t token = 0;
    std::uint32_t batch = 0;

    bool final = false;

    std::vector<std::uint8_t> scene;
};

namespace
{
using bytes = std::vector<std::uint8_t>;
using vec3 = vk::util::math::vec3;

// One archive drives both directions, so field ordering has one definition.
struct invalid_scene : std::runtime_error {
    invalid_scene() : std::runtime_error("Invalid scene payload or batch budget exceeded") {}
};
void require(bool condition) { if (!condition) throw invalid_scene{}; }

template<bool Reading>
struct archive {
    std::conditional_t<Reading, const bytes, bytes>& data;
    std::size_t pos = 0;
    std::size_t left() const { return data.size() - pos; }
    template<class T> void value(T& v) {
        if constexpr (std::is_same_v<std::remove_cv_t<T>, vec3>) {
            (*this)(v.x, v.y, v.z);
        } else if constexpr (requires { v.begin(); v.end(); }) {
            for (auto& item : v) value(item);
        } else {
            if constexpr (Reading) {
                require(left() >= 4);
                const auto bits = net::movement::detail::get(data.data() + pos);
                if constexpr (std::is_floating_point_v<T>) v = std::bit_cast<float>(bits);
                else v = bits;
                pos += 4;
            } else {
                require(data.size() <= max_scene_bytes - 4);
                const auto offset = data.size();
                data.resize(offset + 4);
                if constexpr (std::is_floating_point_v<T>)
                    net::movement::detail::put(data.data() + offset, std::bit_cast<std::uint32_t>(v));
                else net::movement::detail::put(data.data() + offset, v);
            }
            if constexpr (std::is_floating_point_v<std::remove_cv_t<T>>) require(std::isfinite(v));
        }
    }
    template<class... T> void operator()(T&... values) { (value(values), ...); }
    template<class T> void blob(T& target, std::size_t length) {
        if constexpr (Reading) {
            require(length <= left());
            target.assign(data.begin() + pos, data.begin() + pos + length);
            pos += length;
        } else {
            require(length <= max_scene_bytes - data.size());
            data.insert(data.end(), target.begin(), target.end());
        }
    }
    template<class T, class F>
    void items(T& target, std::size_t count, std::size_t minimum, F visit) {
        if constexpr (Reading) {
            require(count <= left() / minimum);
            target.resize(count);
        }
        for (auto& item : target) visit(item);
    }
};

// Batch envelopes use the same network-order words as scene archives.
struct cursor { const bytes& data; std::size_t pos; };
std::size_t left(const cursor& c) { return c.data.size() - c.pos; }
template<class... T> bool take_all(cursor& c, T&... values) {
    if (left(c) < sizeof...(T) * 4) return false;
    net::movement::detail::read_fields(c.data.data() + c.pos, values...);
    c.pos += sizeof...(T) * 4;
    return true;
}
template<class... T> void put_all(bytes& b, const T&... values) {
    const auto offset = b.size();
    b.resize(offset + sizeof...(T) * 4);
    net::movement::detail::write_fields(b.data() + offset, values...);
}
void append(bytes &out, const bytes &src)
{
    out.insert(out.end(), src.begin(), src.end());
}

std::uint32_t hash(const std::uint8_t *src, std::size_t size)
{
    std::uint32_t result = 2166136261u;

    for (std::size_t i = 0; i < size; ++i)
        result = (result ^ src[i]) * 16777619u;

    return result;
}

bool valid_box(const vec3 &a, const vec3 &b)
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z) && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z) && a.x < b.x && a.y < b.y && a.z < b.z;
}

bool valid_light(const ::scene::point_light &l)
{
    return std::isfinite(l.radius) && l.radius > 0 && std::isfinite(l.strength) && l.strength >= 0 && l.color.x >= 0 && l.color.y >= 0 && l.color.z >= 0;
}
} // namespace
namespace {
template<bool Reading, class Scene>
void transfer(archive<Reading>& ar, Scene& scene)
{
    std::uint32_t meshes = scene.meshes.size(), instances = scene.instances.size();
    std::uint32_t textures = scene.textures.size(), skins = scene.skins.size();
    std::uint32_t boxes = scene.collision_boxes.size(), lights = scene.lights.size();
    ar(meshes, instances, textures, skins, boxes, lights);
    require(lights <= 64);
    if constexpr (Reading)
        require(lights <= ar.left()/32 && boxes <= ar.left()/24 && textures <= ar.left()/20 &&
                skins <= ar.left()/16 && meshes <= ar.left()/12 && instances <= ar.left()/108);
    else require(meshes <= max_scene_bytes/12 && instances <= max_scene_bytes/108);

    ar.items(scene.textures, textures, 20, [&](auto& t) {
        std::uint32_t encoding = !t.encoded.empty();
        std::uint32_t length = encoding ? t.encoded.size() : t.pixels.size();
        ar(t.id, t.width, t.height, encoding, length);
        const auto raw = std::uint64_t(t.width) * t.height * 4u;
        require(t.id && t.width && t.height && encoding <= 1 && raw <= max_scene_bytes - 20);
        if constexpr (Reading) {
            require(length <= ar.left());
            if (encoding) {
                require(length <= INT_MAX && t.width <= INT_MAX && t.height <= INT_MAX);
                int w = 0, h = 0, c = 0;
                std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
                    stbi_load_from_memory(ar.data.data() + ar.pos, int(length), &w, &h, &c, 4), stbi_image_free);
                require(pixels && w == int(t.width) && h == int(t.height));
                t.pixels.assign(pixels.get(), pixels.get() + raw);
                ar.pos += length;
            } else {
                require(raw == length);
                ar.blob(t.pixels, length);
            }
        } else {
            require(length && length <= max_scene_bytes - 20 && (encoding || raw == length));
            ar.blob(encoding ? t.encoded : t.pixels, length);
        }
    });
    ar.items(scene.skins, skins, 16, [&](auto& skin) {
        std::uint32_t joints = skin.inverse_bind.size(), length = skin.skeleton.size(), clips = skin.clips.size();
        ar(skin.id, joints, length, clips);
        require(skin.id && joints && joints <= 1024);
        if constexpr (Reading)
            require(length <= ar.left() && std::uint64_t(joints)*64 <= ar.left()-length && clips <= max_scene_bytes/8);
        else require(length);
        ar.blob(skin.skeleton, length);
        ar.items(skin.inverse_bind, joints, 64, [&](auto& matrix) { ar(matrix); });
        ar.items(skin.clips, clips, 8, [&](auto& clip) {
            std::uint32_t name = clip.name.size(), size = clip.bytes.size();
            ar(name);
            require(name <= 255);
            ar.blob(clip.name, name);
            ar(size);
            require(size);
            ar.blob(clip.bytes, size);
        });
    });
    ar.items(scene.meshes, meshes, 12, [&](auto& mesh) {
        std::uint32_t vertices = mesh.vertices.size(), indices = mesh.indices.size();
        ar(mesh.id, mesh.skin_id, vertices, indices);
        require(vertices && indices && indices % 3 == 0);
        if constexpr (Reading)
            require(ar.left() >= 76 && std::uint64_t(vertices)*(mesh.skin_id ? 64 : 32) + std::uint64_t(indices)*4 <= ar.left()-76);
        else require(vertices <= max_scene_bytes/64 && indices <= max_scene_bytes/4);
        auto& m = mesh.surface;
        ar(m.base_color, m.emissive, m.metallic, m.roughness, m.occlusion, m.alpha_cutoff,
           m.base_color_texture, m.metallic_roughness_texture, m.normal_texture, m.occlusion_texture, m.emissive_texture, m.alpha_mode);
        std::uint32_t sided = m.double_sided;
        ar(sided, m.procedural_pattern);
        if constexpr (Reading) {
            require(sided <= 1 && m.alpha_mode <= 2 && m.procedural_pattern <= 1);
            m.double_sided = sided;
        }
        ar.items(mesh.vertices, vertices, mesh.skin_id ? 64 : 32, [&](auto& v) {
            ar(v.position, v.normal, v.u, v.v);
            if (mesh.skin_id) {
                ar(v.joints, v.weights);
                if constexpr (Reading)
                    for (auto weight : v.weights) require(weight >= 0 && weight <= 1);
            }
        });
        ar.items(mesh.indices, indices, 4, [&](auto& index) { ar(index); require(index < vertices); });
    });
    ar.items(scene.instances, instances, 108, [&](auto& i) {
        ar(i.id, i.mesh_id, i.animation, i.animation_tick, i.overlay_animation, i.overlay_root, i.overlay_weight,
           i.pose.position, i.pose.scale, i.pose.rotation.x, i.pose.rotation.y, i.pose.rotation.z, i.pose.rotation.w,
           i.tint, i.pattern, i.rotation_reference, i.optical_strength, i.optical_mattness, i.editor_group);
        if constexpr (Reading)
            require(i.overlay_weight >= 0 && i.overlay_weight <= 1 &&
                    i.tint.x >= 0 && i.tint.x <= 1 && i.tint.y >= 0 && i.tint.y <= 1 && i.tint.z >= 0 && i.tint.z <= 1 &&
                    (i.pattern <= 3 || i.pattern == UINT32_MAX) && i.optical_strength >= 0 && i.optical_strength <= 1 &&
                    i.optical_mattness >= 0 && i.optical_mattness <= 1);
    });
    ar.items(scene.collision_boxes, boxes, 24, [&](auto& b) {
        ar(b.minimum, b.maximum);
        require(valid_box(b.minimum, b.maximum));
    });
    ar.items(scene.lights, lights, 32, [&](auto& l) {
        ar(l.position, l.radius, l.color, l.strength);
        require(valid_light(l));
    });
}
} // namespace

std::vector<std::uint8_t> serialize(const ::scene::state& scene)
{
    bytes out{'S', 'C', 'N', 'B'};
    archive<false> writer{out};
    transfer(writer, scene);
    return out;
}

bool deserialize(const std::vector<std::uint8_t>& bytes, ::scene::state& scene)
{
    if (bytes.size() < 28 || bytes.size() > max_scene_bytes ||
        bytes[0] != 'S' || bytes[1] != 'C' || bytes[2] != 'N' || bytes[3] != 'B') return false;
    try {
        archive<true> reader{bytes, 4};
        ::scene::state result;
        transfer(reader, result);
        if (reader.left()) return false;
        scene = std::move(result);
        return true;
    } catch (const invalid_scene&) { return false; }
}

std::uint32_t checksum(const std::vector<std::uint8_t> &bytes)
{
    return hash(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> encode_batch(const std::vector<std::uint8_t> &bytes, std::uint32_t token, std::uint32_t batch, bool final)
{
    if (bytes.empty() || bytes.size() > max_scene_bytes || !token || batch >= 65535)
        throw std::invalid_argument("invalid scene batch");
    std::vector<std::uint8_t> result{'V', 'G', 'S', 'C', 1, std::uint8_t(final), 0, 0};
    put_all(result, token, batch, checksum(bytes));
    append(result, bytes);

    return result;
}

bool decode_batch(const std::vector<std::uint8_t> &bytes, batch_payload &result)
{
    if (bytes.size() <= 20 || bytes.size() > max_scene_bytes + 20 || bytes[0] != 'V' || bytes[1] != 'G' || bytes[2] != 'S' || bytes[3] != 'C' || bytes[4] != 1 || bytes[5] > 1 || bytes[6] || bytes[7]) return false;
    cursor in{bytes, 8};
    std::uint32_t token = 0, batch = 0, expected = 0;

    if (!take_all(in, token, batch, expected) || !token || batch >= 65535) return false;

    const auto got = hash(bytes.data() + 20, bytes.size() - 20);

    if (expected != got) return false;
    result = {token, batch, bytes[5] != 0, std::vector<std::uint8_t>(bytes.begin() + 20, bytes.end())};

    return true;
}

} // namespace net::scene_stream
