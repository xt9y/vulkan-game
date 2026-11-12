#pragma once
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#include "../net/scene_stream.hpp"
#include "../scene.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <filesystem>
#include <limits>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/memory/unique_ptr.h>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

namespace srv::assets
{


struct rig
{
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;
    std::vector<std::string> names;
    std::vector<std::uint32_t> skin_joints; // glTF skin order -> Ozz order
};

rig import_rig(const fastgltf::Asset &asset, std::size_t skin_index);
::scene::skin serialize_rig(const fastgltf::Asset &asset, std::size_t skin_index, std::uint32_t id, const rig &imported);
namespace
{
std::span<const std::byte> encoded(const fastgltf::Asset &asset, const fastgltf::Image &image)
{
    if (const auto *array = std::get_if<fastgltf::sources::Array>(&image.data))
        return {array->bytes.data(), array->bytes.size()};

    if (const auto *view = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
        const auto &region = asset.bufferViews.at(view->bufferViewIndex);
        const auto &buffer = asset.buffers.at(region.bufferIndex);

        if (const auto *array = std::get_if<fastgltf::sources::Array>(&buffer.data)) {
            if (region.byteOffset > array->bytes.size() || region.byteLength > array->bytes.size() - region.byteOffset)
                throw std::runtime_error("Invalid glTF image buffer view");

            return {array->bytes.data() + region.byteOffset, region.byteLength};
        }
    }

    throw std::runtime_error("Unsupported glTF image source");
}
} // namespace
std::vector<::scene::texture> images(const fastgltf::Asset &asset, std::uint32_t first_texture)
{
    std::vector<::scene::texture> result;
    result.reserve(asset.images.size());

    for (std::size_t i = 0; i < asset.images.size(); ++i)
    {
        const auto source = encoded(asset, asset.images[i]);

        if (source.empty() || source.size() > INT_MAX)
            throw std::runtime_error("Invalid glTF image size");

        int w = 0, h = 0, channels = 0;
        auto *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(source.data()), int(source.size()), &w, &h, &channels, 4);

        if (!pixels || w <= 0 || h <= 0 || std::uint64_t(w) * h * 4 > net::scene_stream::max_scene_bytes - 128) {
            stbi_image_free(pixels);
            throw std::runtime_error("Invalid or oversized glTF texture");
        }
        ::scene::texture texture;
        texture.id = first_texture + std::uint32_t(i);
        texture.width = std::uint32_t(w);
        texture.height = std::uint32_t(h);
        texture.encoded.assign(reinterpret_cast<const std::uint8_t *>(source.data()), reinterpret_cast<const std::uint8_t *>(source.data()) + source.size());
        stbi_image_free(pixels);
        result.push_back(std::move(texture));
    }

    return result;
}

namespace
{
::scene::material material(const fastgltf::Asset &asset, const fastgltf::Primitive &primitive, std::uint32_t first_texture)
{
    ::scene::material result;

    if (!primitive.materialIndex) return result;
    const auto &source = asset.materials.at(*primitive.materialIndex);

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
        const auto &ref = asset.textures.at(texture_index);

        if (!ref.imageIndex)
            throw std::runtime_error("Unsupported compressed glTF texture");

        return first_texture + std::uint32_t(*ref.imageIndex);
    };

    if (source.pbrData.baseColorTexture)
        result.base_color_texture = id(source.pbrData.baseColorTexture->textureIndex);

    if (source.pbrData.metallicRoughnessTexture)
        result.metallic_roughness_texture = id(source.pbrData.metallicRoughnessTexture->textureIndex);

    if (source.normalTexture)
        result.normal_texture = id(source.normalTexture->textureIndex);

    if (source.occlusionTexture)
        result.occlusion_texture = id(source.occlusionTexture->textureIndex);

    if (source.emissiveTexture)
        result.emissive_texture = id(source.emissiveTexture->textureIndex);

    return result;
}

vk::util::math::vec3 position(const fastgltf::math::nmat4x4 &m, fastgltf::math::fvec3 p)
{
    const float x = p[0], y = p[1], z = p[2];

    return {
        float(m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0]), float(m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1]), float(m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2])
    };
}

vk::util::math::vec3 normal(const fastgltf::math::nmat4x4 &m, fastgltf::math::fvec3 n)
{
    using vec3 = vk::util::math::vec3;
    const vec3 a{float(m[0][0]), float(m[0][1]), float(m[0][2])};
    const vec3 b{float(m[1][0]), float(m[1][1]), float(m[1][2])};
    const vec3 c{float(m[2][0]), float(m[2][1]), float(m[2][2])};

    const auto x = cross(b, c);
    const auto y = cross(c, a);
    const auto z = cross(a, b);
    auto transformed = (x * n[0] + y * n[1] + z * n[2]) * (dot(a, x) < 0.0f ? -1.0f : 1.0f);
    const float d = length(transformed);

    return d > 1e-8f ? transformed * (1.f / d) : vec3{0, 1, 0};
}
} // namespace
::scene::state load(const std::filesystem::path &path, std::uint32_t first_mesh, std::uint32_t first_instance, std::uint32_t first_texture, std::uint32_t first_skin)
{
    auto bytes = fastgltf::MappedGltfFile::FromPath(path);
    if (!bytes) throw std::runtime_error("Could not open glTF: " + path.string());
    fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization);
    constexpr auto options = fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages | fastgltf::Options::GenerateMeshIndices | fastgltf::Options::DecomposeNodeMatrices;

    auto parsed = parser.loadGltf(bytes.get(), path.parent_path(), options);

    if (parsed.error() != fastgltf::Error::None)
        throw std::runtime_error(std::string("Invalid glTF: ") + std::string(fastgltf::getErrorMessage(parsed.error())));

    auto asset = std::move(parsed.get());
    ::scene::state result;
    result.textures = images(asset, first_texture);
    std::vector<std::vector<std::uint32_t>> remaps;
    remaps.reserve(asset.skins.size());

    for (std::size_t i = 0; i < asset.skins.size(); ++i)
    {
        auto imported = import_rig(asset, i);
        result.skins.push_back(serialize_rig(asset, i, first_skin + std::uint32_t(i), imported));
        remaps.push_back(std::move(imported.skin_joints));
    }

    if (asset.scenes.empty()) return result;

    const auto scene_index = asset.defaultScene.value_or(std::size_t(0));

    if (scene_index >= asset.scenes.size())
        throw std::runtime_error("Invalid glTF default scene");
    fastgltf::iterateSceneNodes(asset, scene_index, fastgltf::math::nmat4x4{}, [&](fastgltf::Node &node, const fastgltf::math::nmat4x4 &world)
                                {
            if (!node.meshIndex || !node.visible) return;
            const bool skinned = node.skinIndex.has_value();
            const auto& source_mesh = asset.meshes.at(*node.meshIndex);
            for (const auto& primitive : source_mesh.primitives) {
                if (primitive.type != fastgltf::PrimitiveType::Triangles)
                    continue;
                const auto p = primitive.findAttribute("POSITION");
                if (p == primitive.attributes.end() || !primitive.indicesAccessor)
                    throw std::runtime_error("Missing indexed triangle positions");
                const auto& positions = asset.accessors.at(p->accessorIndex);
                const auto n = primitive.findAttribute("NORMAL");
                const auto u = primitive.findAttribute("TEXCOORD_0");
                const auto joints = primitive.findAttribute("JOINTS_0");
                const auto weights = primitive.findAttribute("WEIGHTS_0");
if (skinned && (joints == primitive.attributes.end() || weights == primitive.attributes.end()))
                    throw std::runtime_error("Skin requires JOINTS_0 and WEIGHTS_0");
                ::scene::mesh mesh;
                mesh.id = first_mesh++;
                mesh.skin_id = skinned ? first_skin + std::uint32_t(*node.skinIndex) : 0;
                mesh.surface = material(asset, primitive, first_texture);
                mesh.vertices.resize(positions.count);
                for (std::size_t i = 0; i < positions.count; ++i) {
                    auto& vertex = mesh.vertices[i];
const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>( asset, positions, i);
vertex.position = skinned ? vk::util::math::vec3{p[0], p[1], p[2]} : position(world, p);
                    if (n != primitive.attributes.end()) {
                        const auto normal_value = fastgltf::getAccessorElement<
                            fastgltf::math::fvec3>(asset, asset.accessors[n->accessorIndex], i);
vertex.normal = skinned ? vk::util::math::vec3{normal_value[0], normal_value[1], normal_value[2]} : normal(world, normal_value);
                    } else
                        vertex.normal = {0, 1, 0};
                    if (u != primitive.attributes.end()) {
const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>( asset, asset.accessors[u->accessorIndex], i);
                        vertex.u = uv[0];
                        vertex.v = uv[1];
                    }
                    if (skinned) {
                        const auto indices = fastgltf::getAccessorElement<
fastgltf::math::u16vec4>( asset, asset.accessors[joints->accessorIndex], i);
const auto w = fastgltf::getAccessorElement<fastgltf::math::fvec4>( asset, asset.accessors[weights->accessorIndex], i);
                        const auto& remap = remaps.at(*node.skinIndex);
                        float total = 0;
                        for (int k = 0; k < 4; ++k) {
                            if (indices[k] >= remap.size())
                                throw std::runtime_error("Invalid skin joint index");
                            vertex.joints[k] = remap[indices[k]];
                            vertex.weights[k] = std::max(0.0f, w[k]);
                            total += vertex.weights[k];
                        }
                        if (total <= 0.0f) throw std::runtime_error("Zero skin weights");
                        for (float& weight : vertex.weights) weight /= total;
                    }
                }
                const auto& indices = asset.accessors.at(*primitive.indicesAccessor);
                mesh.indices.resize(indices.count);
                for (std::size_t i = 0; i < indices.count; ++i)
                    mesh.indices[i] = fastgltf::getAccessorElement<std::uint32_t>(asset, indices, i);
                if (mesh.indices.empty() || mesh.indices.size() % 3)
                    throw std::runtime_error("Invalid triangle index count");
                if (n == primitive.attributes.end()) {
                    for (auto& vertex : mesh.vertices)
                        vertex.normal = {0, 0, 0};
                    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                        const auto a = mesh.indices[i];
                        const auto b = mesh.indices[i + 1];
                        const auto c = mesh.indices[i + 2];
if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
                            throw std::runtime_error("Invalid glTF mesh indices");
const auto face = cross( mesh.vertices[b].position - mesh.vertices[a].position, mesh.vertices[c].position - mesh.vertices[a].position );
                        mesh.vertices[a].normal = mesh.vertices[a].normal + face;
                        mesh.vertices[b].normal = mesh.vertices[b].normal + face;
                        mesh.vertices[c].normal = mesh.vertices[c].normal + face;
                    }
                    for (auto& vertex : mesh.vertices) {
                        const auto length_squared = dot(vertex.normal, vertex.normal);
vertex.normal = length_squared > 1e-12f ? vertex.normal * (1.0f / std::sqrt(length_squared)) : vk::util::math::vec3{0, 1, 0};
                    }
                }
                float low_u=std::numeric_limits<float>::infinity();
                float low_v=low_u;
                float high_u=-low_u,high_v=-low_u;
                for(const auto& vertex:mesh.vertices) {
                    low_u=std::min(low_u,vertex.u);
                    low_v=std::min(low_v,vertex.v);
                    high_u=std::max(high_u,vertex.u);
                    high_v=std::max(high_v,vertex.v);
                }
if(u==primitive.attributes.end() || (high_u-low_u<1e-6f && high_v-low_v<1e-6f)) {
                    using vec3=vk::util::math::vec3;
                    const auto component=[](vec3 p,int axis) {
                        return axis==0?p.x:(axis==1?p.y:p.z);
                    };
                    std::array<float,3> lo{
mesh.vertices.front().position.x, mesh.vertices.front().position.y, mesh.vertices.front().position.z};
                    auto hi=lo;
                    for(const auto& vertex:mesh.vertices)
                        for(int axis=0;axis<3;++axis) {
                            const auto value=component(vertex.position,axis);
                            lo[axis]=std::min(lo[axis],value);
                            hi[axis]=std::max(hi[axis],value);
                        }
                    std::array<int,3> axes{0,1,2};
                    std::sort(axes.begin(),axes.end(),[&](int a,int b) {
                        return hi[a]-lo[a]>hi[b]-lo[b];
                    });
                    for(auto& vertex:mesh.vertices) {
vertex.u=(component(vertex.position,axes[0])-lo[axes[0]])/ std::max(hi[axes[0]]-lo[axes[0]],1e-6f)*.98f+.01f;
vertex.v=(component(vertex.position,axes[1])-lo[axes[1]])/ std::max(hi[axes[1]]-lo[axes[1]],1e-6f)*.98f+.01f;
                    }
                }
                ::scene::instance instance;
                instance.id = first_instance++;
                instance.mesh_id = mesh.id;
                instance.animation = 0;
                instance.animation_tick = 0;
                result.meshes.push_back(std::move(mesh));
                result.instances.push_back(instance);
            } });
    return result;
}

namespace
{
using raw_joint = ozz::animation::offline::RawSkeleton::Joint;
using raw_clip = ozz::animation::offline::RawAnimation;

constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
ozz::math::Transform transform(const fastgltf::Node &node)
{
    const auto *trs = std::get_if<fastgltf::TRS>(&node.transform);
    if (!trs) throw std::runtime_error("glTF node matrix must be decomposed");
    ozz::math::Transform value{};
    value.translation = {float(trs->translation[0]), float(trs->translation[1]), float(trs->translation[2])};
    value.rotation = {float(trs->rotation[0]), float(trs->rotation[1]), float(trs->rotation[2]), float(trs->rotation[3])};
    value.scale = {float(trs->scale[0]), float(trs->scale[1]), float(trs->scale[2])};

    return value;
}
} // namespace
namespace
{
template <typename T>
std::vector<std::uint8_t> archive(const T &value)
{
    ozz::io::MemoryStream buffer;
    {
        ozz::io::OArchive writer(&buffer);
        writer << value;
    }

    std::vector<std::uint8_t> bytes(buffer.Size());

    if (buffer.Seek(0, ozz::io::Stream::kSet) || buffer.Read(bytes.data(), bytes.size()) != bytes.size())
        throw std::runtime_error("Ozz archive could not be serialized");

    return bytes;
}
} // namespace
rig import_rig(const fastgltf::Asset &asset, std::size_t skin_index)
{
    const auto &skin = asset.skins.at(skin_index);

    const auto count = asset.nodes.size();
    std::vector<std::size_t> parent(count, none);

    for (std::size_t i = 0; i < count; ++i)
        for (const auto child : asset.nodes[i].children)
        {
            if (child >= count || parent[child] != none)
                throw std::runtime_error("Invalid glTF joint hierarchy");
            parent[child] = i;
        }

    std::vector<bool> included(count, false);

    for (const auto joint : skin.joints)
    {
        if (joint >= count) throw std::runtime_error("Invalid skin joint index");

        auto node = joint;

        for (std::size_t steps = 0; node != none && steps <= count; ++steps)
        {
            included[node] = true;
            node = parent[node];

            if (steps == count)
                throw std::runtime_error("Cyclic glTF joint hierarchy");
        }
    }

    ozz::animation::offline::RawSkeleton raw;
    std::vector<std::size_t> ordered;

    auto append = [&](std::size_t index, auto &&self) -> raw_joint
    {
        raw_joint joint{};
        const auto &name = asset.nodes[index].name;
        joint.name = name.empty() ? ("node_" + std::to_string(index)).c_str() : name.c_str();
        joint.transform = transform(asset.nodes[index]);
        ordered.push_back(index);

        for (auto child : asset.nodes[index].children)
            if (included[child]) joint.children.push_back(self(child, self));

        return joint;
    };

    for (std::size_t i = 0; i < count; ++i)
        if (included[i] && (parent[i] == none || !included[parent[i]]))
            raw.roots.push_back(append(i, append));

    if (!raw.Validate() || raw.num_joints() == 0)
        throw std::runtime_error("Invalid Ozz skeleton");

    rig result;
    result.skeleton = ozz::animation::offline::SkeletonBuilder{}(raw);

    if (!result.skeleton)
        throw std::runtime_error("Ozz skeleton build failed");

    std::unordered_map<std::size_t,std::uint32_t> joint_to_track;
    joint_to_track.reserve(ordered.size());
    for(std::size_t i=0;i<ordered.size();++i)
        joint_to_track.emplace(ordered[i],std::uint32_t(i));
    for(const auto joint:skin.joints) {
        const auto it=joint_to_track.find(joint);
        if(it==joint_to_track.end())
            throw std::runtime_error("Skin remap missing joint");
        result.skin_joints.push_back(it->second);
    }

    for (const auto &source : asset.animations)
    {
        raw_clip clip;
        clip.name = source.name.c_str();
        clip.tracks.resize(ordered.size());

        float duration = 0.f;
        bool has_channels = false;

        for (const auto &channel : source.channels)
        {
            if (!channel.nodeIndex) continue;

            const auto joint=joint_to_track.find(*channel.nodeIndex);
            if(joint==joint_to_track.end() ||
               channel.path==fastgltf::AnimationPath::Weights)continue;
            const auto &sampler = source.samplers.at(channel.samplerIndex);

            if (sampler.interpolation != fastgltf::AnimationInterpolation::Linear)
                throw std::runtime_error("Nonlinear glTF animation needs resampling");
            const auto &inputs = asset.accessors.at(sampler.inputAccessor);
            const auto &outputs = asset.accessors.at(sampler.outputAccessor);

            if (inputs.count != outputs.count)
                throw std::runtime_error("Mismatched glTF animation channels");
            auto& track=clip.tracks[joint->second];

            for (std::size_t i = 0; i < inputs.count; ++i)
            {
                const auto t = fastgltf::getAccessorElement<float>(asset, inputs, i);
                duration = std::max(duration, t);

                if (channel.path == fastgltf::AnimationPath::Translation) {
                    const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, outputs, i);
                    track.translations.push_back({t, {p[0], p[1], p[2]}});
                }
                else if (channel.path == fastgltf::AnimationPath::Rotation) {
                    const auto q = fastgltf::getAccessorElement<fastgltf::math::fvec4>(asset, outputs, i);
                    track.rotations.push_back({t, {q[0], q[1], q[2], q[3]}});
                }
                else if (channel.path == fastgltf::AnimationPath::Scale) {
                    const auto v = fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, outputs, i);
                    track.scales.push_back({t, {v[0], v[1], v[2]}});
                }
            }

            has_channels = true;
        }

        if (!has_channels) continue;
        clip.duration = std::max(duration, 0.001f);

        for (std::size_t i = 0; i < ordered.size(); ++i)
        {
            auto &track = clip.tracks[i];

            const auto pose = transform(asset.nodes[ordered[i]]);

            if (track.translations.empty())
                track.translations.push_back({0, pose.translation});

            if (track.rotations.empty())
                track.rotations.push_back({0, pose.rotation});

            if (track.scales.empty())
                track.scales.push_back({0, pose.scale});
        }

        if (!clip.Validate())
            throw std::runtime_error("Invalid glTF animation after Ozz conversion");

        auto built = ozz::animation::offline::AnimationBuilder{}(clip);
        if (!built) throw std::runtime_error("Ozz animation build failed");
        result.names.emplace_back(source.name);
        result.clips.push_back(std::move(built));
    }

    return result;
}
::scene::skin serialize_rig(const fastgltf::Asset &asset, std::size_t skin_index, std::uint32_t id, const rig &imported)
{
    ::scene::skin result;
    result.id = id;
    result.skeleton = archive(*imported.skeleton);
    result.inverse_bind.resize(imported.skeleton->num_joints());

    for (auto &m : result.inverse_bind)
        m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto &source = asset.skins.at(skin_index);

    if (source.inverseBindMatrices) {
        const auto &accessor = asset.accessors.at(*source.inverseBindMatrices);

        if (accessor.count != imported.skin_joints.size())
            throw std::runtime_error("Mismatched glTF inverse bind matrices");

        for (std::size_t i = 0; i < accessor.count; ++i)
        {
            const auto matrix = fastgltf::getAccessorElement<fastgltf::math::fmat4x4>(asset, accessor, i);

            auto &values = result.inverse_bind[imported.skin_joints[i]];

            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    values[c * 4 + r] = matrix[c][r];
        }
    }

    for (std::size_t i = 0; i < imported.clips.size(); ++i)
        result.clips.push_back({imported.names[i], archive(*imported.clips[i])});

    return result;
}

} // namespace srv::assets
