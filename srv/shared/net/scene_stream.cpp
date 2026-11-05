#include "scene_stream.hpp"
#include <stb_image.h>
#include <climits>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace net::scene_stream
{
    namespace
    {
        void write(std::uint8_t* data, std::uint32_t value)
        {
            data[0] = std::uint8_t(value >> 24);
            data[1] = std::uint8_t(value >> 16);
            data[2] = std::uint8_t(value >> 8);
            data[3] = std::uint8_t(value);
        }
        
        std::uint32_t read(const std::uint8_t* data)
        {
            return (std::uint32_t(data[0]) << 24) |
                   (std::uint32_t(data[1]) << 16) |
                   (std::uint32_t(data[2]) << 8) |
                   std::uint32_t(data[3]);
        }

        void add(std::vector<std::uint8_t>& bytes, std::uint32_t value)
        {
            const auto index = bytes.size();
            bytes.resize(index + 4);
            write(bytes.data() + index, value);
        }

        void add_float(std::vector<std::uint8_t>& bytes, float value)
        {
            if (!std::isfinite(value))
            {
                throw std::runtime_error("Non-finite scene value");
            }

            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            add(bytes, bits);
        }

        struct reader
        {
            const std::vector<std::uint8_t>& bytes;
            std::size_t offset = 0;

            bool take(std::uint32_t& value)
            {
                if (offset > bytes.size() || bytes.size() - offset < 4)
                {
                    return false;
                }

                value = read(bytes.data() + offset);
                offset += 4;
                return true;
            }

            bool take_float(float& value)
            {
                std::uint32_t bits = 0;

                if (!take(bits))
                {
                    return false;
                }

                std::memcpy(&value, &bits, sizeof(value));
                return std::isfinite(value);
            }

            std::size_t remaining() const
            {
                return bytes.size() - offset;
            }
        };

        void add_vec3(std::vector<std::uint8_t>& bytes, vk::util::math::vec3 value)
        {
            add_float(bytes, value.x);
            add_float(bytes, value.y);
            add_float(bytes, value.z);
        }

        bool take_vec3(reader& input, vk::util::math::vec3& value)
        {
            return input.take_float(value.x) &&
                   input.take_float(value.y) &&
                   input.take_float(value.z);
        }
    } // namespace

    std::vector<std::uint8_t> serialize(const ::scene::state& scene)
    {
        // The byte budget, not an arbitrary object count, bounds the snapshot.
        if (scene.meshes.size() > max_scene_bytes / 12 ||
            scene.instances.size() > max_scene_bytes / 48)
        {
            throw std::runtime_error("Scene snapshot exceeds maximum batch size");
        }

        std::vector<std::uint8_t> bytes{'S', 'C', 'N', '8'};

        add(bytes, std::uint32_t(scene.meshes.size()));
        add(bytes, std::uint32_t(scene.instances.size()));
        add(bytes, std::uint32_t(scene.textures.size()));
        add(bytes, std::uint32_t(scene.skins.size()));
        add(bytes, std::uint32_t(scene.collision_boxes.size()));

        for (const auto& texture : scene.textures)
        {
            const auto raw_size = std::uint64_t(texture.width) * texture.height * 4;
            const bool compressed = !texture.encoded.empty();
            const auto& payload = compressed ? texture.encoded : texture.pixels;
            if (texture.id == 0 || !texture.width || !texture.height ||
                raw_size > max_scene_bytes - 20 ||
                (!compressed && raw_size != texture.pixels.size()) ||
                payload.empty() || payload.size() > max_scene_bytes - 20 ||
                bytes.size() + 20 + payload.size() > max_scene_bytes)
                throw std::runtime_error("Invalid texture or scene batch too large");
            add(bytes, texture.id);
            add(bytes, texture.width);
            add(bytes, texture.height);
            add(bytes, compressed ? 1u : 0u);
            add(bytes, std::uint32_t(payload.size()));
            bytes.insert(bytes.end(), payload.begin(), payload.end());
        }

        for (const auto& skin : scene.skins)
        {
            if (!skin.id || skin.skeleton.empty() || skin.inverse_bind.empty() ||
                skin.inverse_bind.size() > 1024)
                throw std::runtime_error("Invalid skeletal asset");
            add(bytes, skin.id);
            add(bytes, std::uint32_t(skin.inverse_bind.size()));
            add(bytes, std::uint32_t(skin.skeleton.size()));
            add(bytes, std::uint32_t(skin.clips.size()));
            bytes.insert(bytes.end(), skin.skeleton.begin(), skin.skeleton.end());
            for (const auto& matrix : skin.inverse_bind)
                for (float value : matrix) add_float(bytes, value);
            for (const auto& clip : skin.clips)
            {
                if (clip.name.size() > 255 || clip.bytes.empty())
                    throw std::runtime_error("Invalid skeletal animation clip");
                add(bytes, std::uint32_t(clip.name.size()));
                bytes.insert(bytes.end(), clip.name.begin(), clip.name.end());
                add(bytes, std::uint32_t(clip.bytes.size()));
                bytes.insert(bytes.end(), clip.bytes.begin(), clip.bytes.end());
            }
            if (bytes.size() > max_scene_bytes)
                throw std::runtime_error("Skeletal asset exceeds scene batch budget");
        }

        for (const auto& mesh : scene.meshes)
        {
            if (mesh.vertices.empty() ||
                mesh.indices.empty() ||
                mesh.indices.size() % 3 ||
                mesh.vertices.size() > max_scene_bytes / 64 ||
                mesh.indices.size() > max_scene_bytes / 4)
            {
                throw std::runtime_error("Invalid scene mesh");
            }

            add(bytes, mesh.id);
            add(bytes, mesh.skin_id);
            add(bytes, std::uint32_t(mesh.vertices.size()));
            add(bytes, std::uint32_t(mesh.indices.size()));

            for (auto value : mesh.surface.base_color) add_float(bytes, value);
            for (auto value : mesh.surface.emissive) add_float(bytes, value);
            add_float(bytes, mesh.surface.metallic);
            add_float(bytes, mesh.surface.roughness);
            add_float(bytes, mesh.surface.occlusion);
            add_float(bytes, mesh.surface.alpha_cutoff);
            add(bytes, mesh.surface.base_color_texture);
            add(bytes, mesh.surface.metallic_roughness_texture);
            add(bytes, mesh.surface.normal_texture);
            add(bytes, mesh.surface.occlusion_texture);
            add(bytes, mesh.surface.emissive_texture);
            add(bytes, mesh.surface.alpha_mode);
            add(bytes, mesh.surface.double_sided ? 1u : 0u);

            for (const auto& vertex : mesh.vertices)
            {
                add_vec3(bytes, vertex.position);
                add_vec3(bytes, vertex.normal);
                add_float(bytes, vertex.u);
                add_float(bytes, vertex.v);
                if (mesh.skin_id)
                {
                    for (const auto joint : vertex.joints) add(bytes, joint);
                    for (const auto weight : vertex.weights) add_float(bytes, weight);
                }
            }

            for (auto index : mesh.indices)
            {
                if (index >= mesh.vertices.size())
                {
                    throw std::runtime_error("Scene mesh index out of bounds");
                }

                add(bytes, index);
            }

            if (bytes.size() > max_scene_bytes)
            {
                throw std::runtime_error("Scene snapshot exceeds maximum batch size");
            }
        }

        for (const auto& instance : scene.instances)
        {
            add(bytes, instance.id);
            add(bytes, instance.mesh_id);
            add(bytes, instance.animation);
            add(bytes, instance.animation_tick);
            add(bytes, instance.overlay_animation);
            add(bytes, instance.overlay_root);
            add_float(bytes, instance.overlay_weight);
            add_vec3(bytes, instance.pose.position);
            add_vec3(bytes, instance.pose.scale);

            add_float(bytes, instance.pose.rotation.x);
            add_float(bytes, instance.pose.rotation.y);
            add_float(bytes, instance.pose.rotation.z);
            add_float(bytes, instance.pose.rotation.w);
        }

        for (const auto& box : scene.collision_boxes)
        {
            const auto& a = box.minimum;
            const auto& b = box.maximum;
            if (!std::isfinite(a.x) || !std::isfinite(a.y) ||
                !std::isfinite(a.z) || !std::isfinite(b.x) ||
                !std::isfinite(b.y) || !std::isfinite(b.z) ||
                a.x >= b.x || a.y >= b.y || a.z >= b.z)
                throw std::runtime_error("Invalid scene collision bounds");
            add_vec3(bytes, a);
            add_vec3(bytes, b);
        }

        if (bytes.size() > max_scene_bytes)
        {
            throw std::runtime_error("Scene snapshot exceeds maximum batch size");
        }

        return bytes;
    }

     bool deserialize(
        const std::vector<std::uint8_t>& bytes,
        ::scene::state& scene
    )
    {
        if (bytes.size() < 24 ||
            bytes.size() > max_scene_bytes ||
            bytes[0] != 'S' ||
            bytes[1] != 'C' ||
            bytes[2] != 'N' ||
            bytes[3] != '8')
        {
            return false;
        }

        reader input{bytes, 4};

        std::uint32_t mesh_count = 0;
        std::uint32_t instance_count = 0;
        std::uint32_t texture_count = 0;
        std::uint32_t skin_count = 0;
        std::uint32_t collider_count = 0;

        if (!input.take(mesh_count) ||
            !input.take(instance_count) ||
            !input.take(texture_count) ||
            !input.take(skin_count) ||
            !input.take(collider_count) ||
            collider_count > input.remaining() / 24 ||
            texture_count > input.remaining() / 20 ||
            skin_count > input.remaining() / 16 ||
            mesh_count > input.remaining() / 12 ||
            instance_count > input.remaining() / 48)
        {
            return false;
        }

        ::scene::state result;
        result.meshes.reserve(mesh_count);
        result.instances.reserve(instance_count);
        result.textures.reserve(texture_count);
        result.skins.reserve(skin_count);
        result.collision_boxes.reserve(collider_count);

        for (std::uint32_t i = 0; i < texture_count; ++i)
        {
            ::scene::texture texture;
            std::uint32_t encoding = 0, bytes_count = 0;
            if (!input.take(texture.id) || !input.take(texture.width) ||
                !input.take(texture.height) || !input.take(encoding) ||
                !input.take(bytes_count) ||
                !texture.id || !texture.width || !texture.height ||
                encoding > 1 || bytes_count > input.remaining() ||
                std::uint64_t(texture.width) * texture.height * 4 > max_scene_bytes - 20)
                return false;

            if (encoding == 0)
            {
                if (std::uint64_t(texture.width) * texture.height * 4 != bytes_count)
                    return false;
                texture.pixels.assign(bytes.begin() + input.offset,
                                      bytes.begin() + input.offset + bytes_count);
            }
            else
            {
                if (bytes_count > INT_MAX || texture.width > INT_MAX ||
                    texture.height > INT_MAX)
                    return false;
                int width = 0, height = 0, channels = 0;
                auto* pixels = stbi_load_from_memory(
                    bytes.data() + input.offset, int(bytes_count),
                    &width, &height, &channels, 4);
                if (!pixels || width != int(texture.width) || height != int(texture.height))
                {
                    stbi_image_free(pixels);
                    return false;
                }
                const auto size = std::size_t(width) * height * 4;
                texture.pixels.assign(pixels, pixels + size);
                stbi_image_free(pixels);
            }
            input.offset += bytes_count;
            result.textures.push_back(std::move(texture));
        }

        for (std::uint32_t i = 0; i < skin_count; ++i)
        {
            ::scene::skin skin;
            std::uint32_t joint_count = 0;
            std::uint32_t skeleton_size = 0;
            std::uint32_t clips = 0;
            if (!input.take(skin.id) || !input.take(joint_count) ||
                !input.take(skeleton_size) || !input.take(clips) ||
                !skin.id || !joint_count || joint_count > 1024 ||
                skeleton_size > input.remaining() ||
                std::uint64_t(joint_count) * 64 > input.remaining() - skeleton_size ||
                clips > max_scene_bytes / 8) return false;
            skin.skeleton.assign(bytes.begin() + input.offset,
                                 bytes.begin() + input.offset + skeleton_size);
            input.offset += skeleton_size;
            skin.inverse_bind.resize(joint_count);
            for (auto& matrix : skin.inverse_bind)
                for (auto& value : matrix)
                    if (!input.take_float(value)) return false;
            for (std::uint32_t j = 0; j < clips; ++j)
            {
                std::uint32_t length = 0, size = 0;
                if (!input.take(length) || length > 255 ||
                    length > input.remaining()) return false;
                ::scene::clip clip;
                clip.name.assign(reinterpret_cast<const char*>(bytes.data() + input.offset), length);
                input.offset += length;
                if (!input.take(size) || !size || size > input.remaining()) return false;
                clip.bytes.assign(bytes.begin() + input.offset,
                                  bytes.begin() + input.offset + size);
                input.offset += size;
                skin.clips.push_back(std::move(clip));
            }
            result.skins.push_back(std::move(skin));
        }

        for (std::uint32_t i = 0; i < mesh_count; ++i)
        {
            ::scene::mesh mesh;
            std::uint32_t vertices = 0;
            std::uint32_t indices = 0;

            if (!input.take(mesh.id) ||
                !input.take(mesh.skin_id) ||
                !input.take(vertices) ||
                !input.take(indices) ||
                vertices == 0 || indices == 0 || indices % 3 ||
                input.remaining() < 72 ||
                std::uint64_t(vertices) * (mesh.skin_id ? 64 : 32) +
                    std::uint64_t(indices) * 4 > input.remaining() - 72)
            {
                return false;
            }

            if (input.remaining() < 72) return false;
            for (auto& value : mesh.surface.base_color)
                if (!input.take_float(value)) return false;
            for (auto& value : mesh.surface.emissive)
                if (!input.take_float(value)) return false;
            if (!input.take_float(mesh.surface.metallic) ||
                !input.take_float(mesh.surface.roughness) ||
                !input.take_float(mesh.surface.occlusion) ||
                !input.take_float(mesh.surface.alpha_cutoff) ||
                !input.take(mesh.surface.base_color_texture) ||
                !input.take(mesh.surface.metallic_roughness_texture) ||
                !input.take(mesh.surface.normal_texture) ||
                !input.take(mesh.surface.occlusion_texture) ||
                !input.take(mesh.surface.emissive_texture) ||
                !input.take(mesh.surface.alpha_mode)) return false;
            std::uint32_t double_sided = 0;
            if (!input.take(double_sided) || double_sided > 1 ||
                mesh.surface.alpha_mode > 2) return false;
            mesh.surface.double_sided = double_sided != 0;

            mesh.vertices.resize(vertices);
            mesh.indices.resize(indices);

            for (auto& vertex : mesh.vertices)
            {
                if (!take_vec3(input, vertex.position) ||
                    !take_vec3(input, vertex.normal) ||
                    !input.take_float(vertex.u) ||
                    !input.take_float(vertex.v))
                {
                    return false;
                }
                if (mesh.skin_id)
                {
                    for (auto& joint : vertex.joints)
                        if (!input.take(joint)) return false;
                    for (auto& weight : vertex.weights)
                        if (!input.take_float(weight) || weight < 0 || weight > 1) return false;
                }
            }

            for (auto& index : mesh.indices)
            {
                if (!input.take(index) || index >= vertices)
                {
                    return false;
                }
            }

            result.meshes.push_back(std::move(mesh));
        }

        if (instance_count > input.remaining() / 48)
        {
            return false;
        }

        for (std::uint32_t i = 0; i < instance_count; ++i)
        {
            ::scene::instance instance;

            if (!input.take(instance.id) ||
                !input.take(instance.mesh_id) ||
                !input.take(instance.animation) ||
                !input.take(instance.animation_tick) ||
                !input.take(instance.overlay_animation) ||
                !input.take(instance.overlay_root) ||
                !input.take_float(instance.overlay_weight) ||
                instance.overlay_weight < 0.f || instance.overlay_weight > 1.f ||
                !take_vec3(input, instance.pose.position) ||
                !take_vec3(input, instance.pose.scale) ||
                !input.take_float(instance.pose.rotation.x) ||
                !input.take_float(instance.pose.rotation.y) ||
                !input.take_float(instance.pose.rotation.z) ||
                !input.take_float(instance.pose.rotation.w))
            {
                return false;
            }

            result.instances.push_back(instance);
        }

        for (std::uint32_t i = 0; i < collider_count; ++i)
        {
            ::scene::collision_box box{};
            if (!take_vec3(input, box.minimum) ||
                !take_vec3(input, box.maximum))
                return false;
            const auto& a = box.minimum;
            const auto& b = box.maximum;
            if (!std::isfinite(a.x) || !std::isfinite(a.y) ||
                !std::isfinite(a.z) || !std::isfinite(b.x) ||
                !std::isfinite(b.y) || !std::isfinite(b.z) ||
                a.x >= b.x || a.y >= b.y || a.z >= b.z)
                return false;
            result.collision_boxes.push_back(box);
        }

        if (input.remaining() != 0)
        {
            return false;
        }

        scene = std::move(result);
        return true;
    }

    std::uint32_t checksum(const std::vector<std::uint8_t>& bytes)
    {
        std::uint32_t hash = 2166136261u;

        for (auto byte : bytes)
        {
            hash = (hash ^ byte) * 16777619u;
        }

        return hash;
    }
    
    std::size_t encode_fragment(const std::vector<std::uint8_t>& bytes, std::uint32_t token, std::uint32_t index, std::uint32_t hash, packet& output, bool final)
    {
        if (bytes.empty() || bytes.size() > max_scene_bytes ||
            std::size_t(index) >= (bytes.size() + fragment_size - 1) / fragment_size)
        {
            return 0;
        }

        const std::size_t offset = std::size_t(index) * fragment_size;
        const std::size_t count = std::min(fragment_size, bytes.size() - offset);

        output = {};

        output[0] = 'V';
        output[1] = 'S';
        output[2] = 'C';
        output[3] = 'N';
        output[4] = 4;
        output[5] = final ? 1 : 0;

        write(output.data() + 8, token);
        write(output.data() + 12, std::uint32_t(bytes.size()));
        write(output.data() + 16, index);
        write(output.data() + 20, hash);

        std::memcpy(output.data() + header_size, bytes.data() + offset, count);

        return header_size + count;
    }

    bool decode_fragment(const std::uint8_t* bytes, std::size_t size, fragment& output)
    {
        if (size <= header_size || size > packet_size ||
            bytes[0] != 'V' || bytes[1] != 'S' ||
            bytes[2] != 'C' || bytes[3] != 'N' ||
            bytes[4] != 4 || bytes[5] > 1 ||
            bytes[6] || bytes[7])
        {
            return false;
        }

        const auto total = read(bytes + 12);
        const auto index = read(bytes + 16);
        const std::size_t part = index & 0xffffu;

        if (total == 0 || total > max_scene_bytes ||
            part >= (std::size_t(total) + fragment_size - 1) / fragment_size ||
            size - header_size != std::min(fragment_size, std::size_t(total) - part * fragment_size))
        {
            return false;
        }

        output = {
            read(bytes + 8),
            total,
            index,
            read(bytes + 20),
            bytes[5] != 0,
            bytes + header_size,
            size - header_size
        };

        return true;
    }
    
} // namespace net::scene_stream
