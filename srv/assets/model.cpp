#include "model.hpp"
#include "image.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace srv::assets
{
    namespace
    {
        ::scene::material material(const fastgltf::Asset& asset,
                                   const fastgltf::Primitive& primitive, std::uint32_t first_texture)
        {
            ::scene::material result;
            if (!primitive.materialIndex) return result;
            const auto& source = asset.materials.at(*primitive.materialIndex);
            for (int i = 0; i < 4; ++i)
                result.base_color[i] = float(source.pbrData.baseColorFactor[i]);
            for (int i = 0; i < 3; ++i)
                result.emissive[i] = float(source.emissiveFactor[i]);
            result.metallic = float(source.pbrData.metallicFactor);
            result.roughness = std::max(0.045f, float(source.pbrData.roughnessFactor));
            result.alpha_cutoff = float(source.alphaCutoff);
            result.alpha_mode = std::uint32_t(source.alphaMode);
            result.double_sided = source.doubleSided;
            const auto id = [&](std::size_t texture_index) -> std::uint32_t
            {
                const auto& ref = asset.textures.at(texture_index);
                if (!ref.imageIndex)
                    throw std::runtime_error("Unsupported compressed glTF texture");
                return first_texture + std::uint32_t(*ref.imageIndex);
            };
            if (source.pbrData.baseColorTexture)
                result.base_color_texture = id(source.pbrData.baseColorTexture->textureIndex);
            if (source.pbrData.metallicRoughnessTexture)
                result.metallic_roughness_texture = id(
                    source.pbrData.metallicRoughnessTexture->textureIndex);
            if (source.normalTexture)
                result.normal_texture = id(source.normalTexture->textureIndex);
            if (source.occlusionTexture)
                result.occlusion_texture = id(source.occlusionTexture->textureIndex);
            if (source.emissiveTexture)
                result.emissive_texture = id(source.emissiveTexture->textureIndex);
            return result;
        }

        vk::util::math::vec3 position(const fastgltf::math::nmat4x4& m,
                                      fastgltf::math::fvec3 p)
        {
            const float x = p[0], y = p[1], z = p[2];
            return {
                float(m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0]),
                float(m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1]),
                float(m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2])
            };
        }

        vk::util::math::vec3 normal(const fastgltf::math::nmat4x4& m,
                                    fastgltf::math::fvec3 n)
        {
            // Transform normals with the inverse transpose, including mirrored scale.
            using vec3 = vk::util::math::vec3;
            const vec3 a{float(m[0][0]), float(m[0][1]), float(m[0][2])};
            const vec3 b{float(m[1][0]), float(m[1][1]), float(m[1][2])};
            const vec3 c{float(m[2][0]), float(m[2][1]), float(m[2][2])};
            const auto x = cross(b, c);
            const auto y = cross(c, a);
            const auto z = cross(a, b);
            auto transformed = (x * n[0] + y * n[1] + z * n[2]) *
                (dot(a, x) < 0.0f ? -1.0f : 1.0f);
            const float d = length(transformed);
            return d > 1e-8f ? transformed * (1.f / d) : vec3{0, 1, 0};
        }
    } // namespace

    ::scene::state load(const std::filesystem::path& path,
                        std::uint32_t first_mesh, std::uint32_t first_instance)
    {
        auto bytes = fastgltf::MappedGltfFile::FromPath(path);
        if (!bytes) throw std::runtime_error("Could not open glTF: " + path.string());

        fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization);
        constexpr auto options = fastgltf::Options::LoadExternalBuffers |
            fastgltf::Options::LoadExternalImages |
            fastgltf::Options::GenerateMeshIndices |
            fastgltf::Options::DecomposeNodeMatrices;

        auto parsed = parser.loadGltf(bytes.get(), path.parent_path(), options);
        if (parsed.error() != fastgltf::Error::None)
            throw std::runtime_error(std::string("Invalid glTF: ") +
                                     std::string(fastgltf::getErrorMessage(parsed.error())));

        auto asset = std::move(parsed.get());
        ::scene::state result;
        result.textures = images(asset, 1000);
        if (asset.scenes.empty()) return result;

        const auto scene_index = asset.defaultScene.value_or(std::size_t(0));
        if (scene_index >= asset.scenes.size())
            throw std::runtime_error("Invalid glTF default scene");

        fastgltf::iterateSceneNodes(asset, scene_index, fastgltf::math::nmat4x4{},
            [&](fastgltf::Node& node, const fastgltf::math::nmat4x4& world)
        {
            if (!node.meshIndex || !node.visible) return;
            if (node.skinIndex)
                throw std::runtime_error("Skinned glTF needs animation asset import");

            const auto& source_mesh = asset.meshes.at(*node.meshIndex);
            for (const auto& primitive : source_mesh.primitives)
            {
                if (primitive.type != fastgltf::PrimitiveType::Triangles)
                    continue;
                const auto p = primitive.findAttribute("POSITION");
                if (p == primitive.attributes.end() || !primitive.indicesAccessor)
                    throw std::runtime_error("Missing indexed triangle positions");

                const auto& positions = asset.accessors.at(p->accessorIndex);
                const auto n = primitive.findAttribute("NORMAL");
                const auto u = primitive.findAttribute("TEXCOORD_0");

                ::scene::mesh mesh;
                mesh.id = first_mesh++;
                mesh.surface = material(asset, primitive, 1000);
                mesh.vertices.resize(positions.count);

                for (std::size_t i = 0; i < positions.count; ++i)
                {
                    auto& vertex = mesh.vertices[i];
                    vertex.position = position(world,
                        fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, positions, i));
                    if (n != primitive.attributes.end())
                        vertex.normal = normal(world,
                            fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                                asset, asset.accessors[n->accessorIndex], i));
                    else
                        vertex.normal = {0, 1, 0};
                    if (u != primitive.attributes.end())
                    {
                        const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>(
                            asset, asset.accessors[u->accessorIndex], i);
                        vertex.u = uv[0];
                        vertex.v = uv[1];
                    }
                }

                const auto& indices = asset.accessors.at(*primitive.indicesAccessor);
                mesh.indices.resize(indices.count);
                for (std::size_t i = 0; i < indices.count; ++i)
                    mesh.indices[i] = fastgltf::getAccessorElement<std::uint32_t>(asset, indices, i);

                if (mesh.indices.empty() || mesh.indices.size() % 3)
                    throw std::runtime_error("Invalid triangle index count");

                if (n == primitive.attributes.end())
                {
                    for (auto& vertex : mesh.vertices)
                        vertex.normal = {0, 0, 0};
                    for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
                    {
                        const auto a = mesh.indices[i];
                        const auto b = mesh.indices[i + 1];
                        const auto c = mesh.indices[i + 2];
                        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() ||
                            c >= mesh.vertices.size())
                            throw std::runtime_error("Invalid glTF mesh indices");
                        const auto face = cross(
                            mesh.vertices[b].position - mesh.vertices[a].position,
                            mesh.vertices[c].position - mesh.vertices[a].position
                        );
                        mesh.vertices[a].normal = mesh.vertices[a].normal + face;
                        mesh.vertices[b].normal = mesh.vertices[b].normal + face;
                        mesh.vertices[c].normal = mesh.vertices[c].normal + face;
                    }
                    for (auto& vertex : mesh.vertices)
                    {
                        const auto length_squared = dot(vertex.normal, vertex.normal);
                        vertex.normal = length_squared > 1e-12f ?
                            vertex.normal * (1.0f / std::sqrt(length_squared)) :
                            vk::util::math::vec3{0, 1, 0};
                    }
                }

                ::scene::instance instance;
                instance.id = first_instance++;
                instance.mesh_id = mesh.id;
                result.meshes.push_back(std::move(mesh));
                result.instances.push_back(instance);
            }
        });
        return result;
    }
} // namespace srv::assets
