#include "scene_stream.hpp"

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
        if (scene.meshes.size() > 65536 ||
            scene.instances.size() > 65536)
        {
            throw std::runtime_error("Scene item count exceeds limit");
        }

        std::vector<std::uint8_t> bytes{'S', 'C', 'N', '1'};

        add(bytes, std::uint32_t(scene.meshes.size()));
        add(bytes, std::uint32_t(scene.instances.size()));

        for (const auto& mesh : scene.meshes)
        {
            if (mesh.vertices.empty() ||
                mesh.indices.empty() ||
                mesh.indices.size() % 3 ||
                mesh.vertices.size() > max_scene_bytes / 32 ||
                mesh.indices.size() > max_scene_bytes / 4)
            {
                throw std::runtime_error("Invalid scene mesh");
            }

            add(bytes, mesh.id);
            add(bytes, std::uint32_t(mesh.vertices.size()));
            add(bytes, std::uint32_t(mesh.indices.size()));

            for (const auto& vertex : mesh.vertices)
            {
                add_vec3(bytes, vertex.position);
                add_vec3(bytes, vertex.normal);
                add_float(bytes, vertex.u);
                add_float(bytes, vertex.v);
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
                throw std::runtime_error("Scene snapshot exceeds 8 MiB");
            }
        }

        for (const auto& instance : scene.instances)
        {
            add(bytes, instance.id);
            add(bytes, instance.mesh_id);
            add_vec3(bytes, instance.pose.position);
            add_vec3(bytes, instance.pose.scale);

            add_float(bytes, instance.pose.rotation.x);
            add_float(bytes, instance.pose.rotation.y);
            add_float(bytes, instance.pose.rotation.z);
            add_float(bytes, instance.pose.rotation.w);
        }

        if (bytes.size() > max_scene_bytes)
        {
            throw std::runtime_error("Scene snapshot exceeds 8 MiB");
        }

        return bytes;
    }

     bool deserialize(
        const std::vector<std::uint8_t>& bytes,
        ::scene::state& scene
    )
    {
        if (bytes.size() < 12 ||
            bytes.size() > max_scene_bytes ||
            bytes[0] != 'S' ||
            bytes[1] != 'C' ||
            bytes[2] != 'N' ||
            bytes[3] != '1')
        {
            return false;
        }

        reader input{bytes, 4};

        std::uint32_t mesh_count = 0;
        std::uint32_t instance_count = 0;

        if (!input.take(mesh_count) ||
            !input.take(instance_count) ||
            mesh_count > 65536 ||
            instance_count > 65536)
        {
            return false;
        }

        ::scene::state result;
        result.meshes.reserve(mesh_count);
        result.instances.reserve(instance_count);

        for (std::uint32_t i = 0; i < mesh_count; ++i)
        {
            ::scene::mesh mesh;
            std::uint32_t vertices = 0;
            std::uint32_t indices = 0;

            if (!input.take(mesh.id) ||
                !input.take(vertices) ||
                !input.take(indices) ||
                vertices == 0 || indices == 0 || indices % 3 ||
                vertices > input.remaining() / 32 ||
                indices > (input.remaining() - std::size_t(vertices) * 32) / 4)
            {
                return false;
            }

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
    
    std::size_t encode_fragment(const std::vector<std::uint8_t>& bytes, std::uint32_t token, std::uint32_t index, std::uint32_t hash, packet& output)
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
        output[4] = 1;

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
            bytes[4] != 1 || bytes[5] ||
            bytes[6] || bytes[7])
        {
            return false;
        }

        const auto total = read(bytes + 12);
        const auto index = read(bytes + 16);

        if (total == 0 || total > max_scene_bytes ||
            std::size_t(index) >= (std::size_t(total) + fragment_size - 1) / fragment_size ||
            size - header_size != std::min(fragment_size, std::size_t(total) - std::size_t(index) * fragment_size))
        {
            return false;
        }

        output = {
            read(bytes + 8),
            total,
            index,
            read(bytes + 20),
            bytes + header_size,
            size - header_size
        };

        return true;
    }
    
} // namespace net::scene_stream
