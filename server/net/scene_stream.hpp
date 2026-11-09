#pragma once
#include "../scene.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

struct cursor
{
    const bytes &data;
    std::size_t pos;
};

std::size_t left(const cursor &c) { return c.data.size() - c.pos; }
void put(bytes &b, std::uint32_t value)
{
    const auto offset = b.size();
    b.resize(offset + 4);
    net::movement::detail::put(b.data() + offset, value);
}

void put(bytes &b, float value)
{
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite scene value");
    put(b, std::bit_cast<std::uint32_t>(value));
}

void put(bytes &b, vec3 value)
{
    put(b, value.x);
    put(b, value.y);
    put(b, value.z);
}

template <class... T>
void put_all(bytes &b, const T &...values) { (put(b, values), ...); }
bool take(cursor &c, std::uint32_t &value)
{
    if (left(c) < 4) return false;
    value = net::movement::detail::get(c.data.data() + c.pos);
    c.pos += 4;

    return true;
}

bool take(cursor &c, float &value)
{
    std::uint32_t bits = 0;

    if (!take(c, bits)) return false;
    value = std::bit_cast<float>(bits);

    return std::isfinite(value);
}

bool take(cursor &c, vec3 &value)
{
    return take(c, value.x) && take(c, value.y) && take(c, value.z);
}

template <class... T>
bool take_all(cursor &c, T &...values) { return (take(c, values) && ...); }
void material_put(bytes &out, const ::scene::material &m)
{
    for (auto x : m.base_color)
        put(out, x);

    for (auto x : m.emissive)
        put(out, x);
    put_all(out, m.metallic, m.roughness, m.occlusion, m.alpha_cutoff, m.base_color_texture, m.metallic_roughness_texture, m.normal_texture, m.occlusion_texture, m.emissive_texture,
            m.alpha_mode, std::uint32_t(m.double_sided), m.procedural_pattern);
}

bool material_take(cursor &in, ::scene::material &m)
{
    for (auto &x : m.base_color)
        if (!take(in, x)) return false;

    for (auto &x : m.emissive)
        if (!take(in, x)) return false;
    std::uint32_t double_sided = 0;

    if (!take_all(in, m.metallic, m.roughness, m.occlusion, m.alpha_cutoff, m.base_color_texture, m.metallic_roughness_texture, m.normal_texture, m.occlusion_texture, m.emissive_texture, m.alpha_mode,
                  double_sided, m.procedural_pattern)) return false;
    if (double_sided > 1 || m.alpha_mode > 2 || m.procedural_pattern > 1) return false;
    m.double_sided = double_sided != 0;

    return true;
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
std::vector<std::uint8_t> serialize(const ::scene::state &scene)
{
    if (scene.meshes.size() > max_scene_bytes / 12 || scene.instances.size() > max_scene_bytes / 96 || scene.lights.size() > 64)
        throw std::runtime_error("Scene snapshot exceeds maximum batch size");
    bytes out{'S', 'C', 'N', 'B'};
    put_all(out, std::uint32_t(scene.meshes.size()), std::uint32_t(scene.instances.size()), std::uint32_t(scene.textures.size()), std::uint32_t(scene.skins.size()), std::uint32_t(scene.collision_boxes.size()),
            std::uint32_t(scene.lights.size()));
    for (const auto &t : scene.textures)
    {
        const bool encoded = !t.encoded.empty();
        const auto &payload = encoded ? t.encoded : t.pixels;

        const auto raw = std::uint64_t(t.width) * t.height * 4u;

        if (!t.id || !t.width || !t.height || raw > max_scene_bytes - 20 || (!encoded && raw != t.pixels.size()) || payload.empty() || payload.size() > max_scene_bytes - 20 || out.size() + 20 + payload.size() > max_scene_bytes)
            throw std::runtime_error("Invalid texture or scene batch too large");
        put_all(out, t.id, t.width, t.height, std::uint32_t(encoded), std::uint32_t(payload.size()));
        append(out, payload);
    }

    for (const auto &skin : scene.skins)
    {
        if (!skin.id || skin.skeleton.empty() || skin.inverse_bind.empty() || skin.inverse_bind.size() > 1024)
            throw std::runtime_error("Invalid skeletal asset");
        put_all(out, skin.id, std::uint32_t(skin.inverse_bind.size()), std::uint32_t(skin.skeleton.size()), std::uint32_t(skin.clips.size()));
        append(out, skin.skeleton);

        for (const auto &matrix : skin.inverse_bind)
            for (float value : matrix)
                put(out, value);

        for (const auto &clip : skin.clips)
        {
            if (clip.name.size() > 255 || clip.bytes.empty())
                throw std::runtime_error("Invalid skeletal animation clip");
            put(out, std::uint32_t(clip.name.size()));
            out.insert(out.end(), clip.name.begin(), clip.name.end());
            put(out, std::uint32_t(clip.bytes.size()));
            append(out, clip.bytes);
        }

        if (out.size() > max_scene_bytes)
            throw std::runtime_error("Skeletal asset exceeds scene batch budget");
    }

    for (const auto &m : scene.meshes)
    {
        if (m.vertices.empty() || m.indices.empty() || m.indices.size() % 3 || m.vertices.size() > max_scene_bytes / 64 || m.indices.size() > max_scene_bytes / 4)
            throw std::runtime_error("Invalid scene mesh");
        put_all(out, m.id, m.skin_id, std::uint32_t(m.vertices.size()), std::uint32_t(m.indices.size()));
        material_put(out, m.surface);

        for (const auto &v : m.vertices)
        {
            put_all(out, v.position, v.normal, v.u, v.v);

            if (m.skin_id) {
                for (auto joint : v.joints)
                    put(out, joint);

                for (auto weight : v.weights)
                    put(out, weight);
            }
        }

        for (auto index : m.indices)
        {
            if (index >= m.vertices.size())
                throw std::runtime_error("Scene mesh index out of bounds");
            put(out, index);
        }

        if (out.size() > max_scene_bytes)
            throw std::runtime_error("Scene snapshot exceeds maximum batch size");
    }

    for (const auto &i : scene.instances)
        put_all(out, i.id, i.mesh_id, i.animation, i.animation_tick, i.overlay_animation, i.overlay_root, i.overlay_weight, i.pose.position, i.pose.scale, i.pose.rotation.x, i.pose.rotation.y, i.pose.rotation.z, i.pose.rotation.w, i.tint, i.pattern, i.rotation_reference);

    for (const auto &b : scene.collision_boxes)
    {
        if (!valid_box(b.minimum, b.maximum))
            throw std::runtime_error("Invalid scene collision bounds");
        put_all(out, b.minimum, b.maximum);
    }

    for (const auto &l : scene.lights)
    {
        if (!valid_light(l)) throw std::runtime_error("Invalid scene light");
        put_all(out, l.position, l.radius, l.color, l.strength);
    }

    if (out.size() > max_scene_bytes)
        throw std::runtime_error("Scene snapshot exceeds maximum batch size");

    return out;
}

bool deserialize(const std::vector<std::uint8_t> &bytes, ::scene::state &scene)
{
    if (bytes.size() < 28 || bytes.size() > max_scene_bytes || bytes[0] != 'S' || bytes[1] != 'C' || bytes[2] != 'N' || bytes[3] != 'B')
        return false;
    cursor in{bytes, 4};
    std::uint32_t meshes = 0, instances = 0, textures = 0, skins = 0, boxes = 0, lights = 0;

    if (!take_all(in, meshes, instances, textures, skins, boxes, lights) || lights > 64 || lights > left(in) / 32 || boxes > left(in) / 24 || textures > left(in) / 20 || skins > left(in) / 16 || meshes > left(in) / 12 || instances > left(in) / 96) return false;
    ::scene::state result;
    result.meshes.reserve(meshes);
    result.instances.reserve(instances);
    result.textures.reserve(textures);
    result.skins.reserve(skins);
    result.collision_boxes.reserve(boxes);
    result.lights.reserve(lights);

    for (std::uint32_t i = 0; i < textures; ++i)
    {
        ::scene::texture t;
        std::uint32_t encoding = 0, length = 0;

        if (!take_all(in, t.id, t.width, t.height, encoding, length) || !t.id || !t.width || !t.height || encoding > 1 || length > left(in) || std::uint64_t(t.width) * t.height * 4u > max_scene_bytes - 20) return false;
        if (encoding == 0) {
            if (std::uint64_t(t.width) * t.height * 4u != length) return false;
            t.pixels.assign(bytes.begin() + in.pos, bytes.begin() + in.pos + length);
        }
        else {
            if (length > INT_MAX || t.width > INT_MAX || t.height > INT_MAX) return false;

            int w = 0, h = 0, c = 0;
            auto *p = stbi_load_from_memory(bytes.data() + in.pos, int(length), &w, &h, &c, 4);

            if (!p || w != int(t.width) || h != int(t.height)) {
                stbi_image_free(p);

                return false;
            }

            t.pixels.assign(p, p + std::size_t(w) * h * 4u);
            stbi_image_free(p);
        }

        in.pos += length;
        result.textures.push_back(std::move(t));
    }

    for (std::uint32_t i = 0; i < skins; ++i)
    {
        ::scene::skin skin;
        std::uint32_t joints = 0, length = 0, clips = 0;

        if (!take_all(in, skin.id, joints, length, clips) || !skin.id || !joints || joints > 1024 || length > left(in) || std::uint64_t(joints) * 64 > left(in) - length || clips > max_scene_bytes / 8) return false;
        skin.skeleton.assign(bytes.begin() + in.pos, bytes.begin() + in.pos + length);
        in.pos += length;
        skin.inverse_bind.resize(joints);

        for (auto &matrix : skin.inverse_bind)
            for (auto &value : matrix)
                if (!take(in, value)) return false;

        for (std::uint32_t j = 0; j < clips; ++j)
        {
            std::uint32_t name = 0, size = 0;

            if (!take(in, name) || name > 255 || name > left(in)) return false;
            ::scene::clip clip;
            clip.name.assign(reinterpret_cast<const char *>(bytes.data() + in.pos), name);
            in.pos += name;

            if (!take(in, size) || !size || size > left(in)) return false;
            clip.bytes.assign(bytes.begin() + in.pos, bytes.begin() + in.pos + size);
            in.pos += size;
            skin.clips.push_back(std::move(clip));
        }

        result.skins.push_back(std::move(skin));
    }

    for (std::uint32_t i = 0; i < meshes; ++i)
    {
        ::scene::mesh mesh;
        std::uint32_t vertices = 0, indices = 0;

        if (!take_all(in, mesh.id, mesh.skin_id, vertices, indices) || !vertices || !indices || indices % 3 || left(in) < 76 || std::uint64_t(vertices) * (mesh.skin_id ? 64u : 32u) + std::uint64_t(indices) * 4u > left(in) - 76) return false;
        if (!material_take(in, mesh.surface)) return false;
        mesh.vertices.resize(vertices);
        mesh.indices.resize(indices);

        for (auto &v : mesh.vertices)
        {
            if (!take_all(in, v.position, v.normal, v.u, v.v)) return false;
            if (mesh.skin_id) {
                for (auto &joint : v.joints)
                    if (!take(in, joint)) return false;

                for (auto &weight : v.weights)
                    if (!take(in, weight) || weight < 0 || weight > 1) return false;
            }
        }

        for (auto &index : mesh.indices)
            if (!take(in, index) || index >= vertices) return false;
        result.meshes.push_back(std::move(mesh));
    }

    if (instances > left(in) / 96) return false;

    for (std::uint32_t i = 0; i < instances; ++i)
    {
        ::scene::instance item;

        if (!take_all(in, item.id, item.mesh_id, item.animation, item.animation_tick, item.overlay_animation, item.overlay_root, item.overlay_weight, item.pose.position, item.pose.scale, item.pose.rotation.x,
                      item.pose.rotation.y, item.pose.rotation.z, item.pose.rotation.w, item.tint, item.pattern, item.rotation_reference) ||
            item.overlay_weight < 0 || item.overlay_weight > 1 ||
            item.tint.x < 0 || item.tint.x > 1 || item.tint.y < 0 || item.tint.y > 1 || item.tint.z < 0 || item.tint.z > 1 ||
            (item.pattern > 1 && item.pattern != UINT32_MAX)) return false;
        result.instances.push_back(std::move(item));
    }

    for (std::uint32_t i = 0; i < boxes; ++i)
    {
        ::scene::collision_box b{};

        if (!take_all(in, b.minimum, b.maximum) || !valid_box(b.minimum, b.maximum))
            return false;
        result.collision_boxes.push_back(b);
    }

    for (std::uint32_t i = 0; i < lights; ++i)
    {
        ::scene::point_light l{};

        if (!take_all(in, l.position, l.radius, l.color, l.strength) || !valid_light(l))
            return false;
        result.lights.push_back(l);
    }

    if (left(in)) return false;
    scene = std::move(result);

    return true;
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
