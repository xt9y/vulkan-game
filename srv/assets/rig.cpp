#include "rig.hpp"

#include <fastgltf/tools.hpp>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace srv::assets
{
    namespace
    {
        using raw_joint = ozz::animation::offline::RawSkeleton::Joint;
        using raw_clip = ozz::animation::offline::RawAnimation;
        constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

        ozz::math::Transform transform(const fastgltf::Node& node)
        {
            const auto* trs = std::get_if<fastgltf::TRS>(&node.transform);
            if (!trs) throw std::runtime_error("glTF node matrix must be decomposed");
            ozz::math::Transform value{};
            value.translation = {float(trs->translation[0]), float(trs->translation[1]),
                                 float(trs->translation[2])};
            value.rotation = {float(trs->rotation[0]), float(trs->rotation[1]),
                              float(trs->rotation[2]), float(trs->rotation[3])};
            value.scale = {float(trs->scale[0]), float(trs->scale[1]),
                           float(trs->scale[2])};
            return value;
        }
    } // namespace

    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index)
    {
        const auto& skin = asset.skins.at(skin_index);
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
        auto append = [&](std::size_t index, auto&& self) -> raw_joint
        {
            raw_joint joint{};
            joint.name = ("node_" + std::to_string(index)).c_str();
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

        for (auto joint : skin.joints)
        {
            auto it = std::find(ordered.begin(), ordered.end(), joint);
            if (it == ordered.end()) throw std::runtime_error("Skin remap missing joint");
            result.skin_joints.push_back(std::uint32_t(it - ordered.begin()));
        }

        for (const auto& source : asset.animations)
        {
            raw_clip clip;
            clip.name = source.name.c_str();
            clip.tracks.resize(ordered.size());
            float duration = 0.f;
            bool has_channels = false;

            for (const auto& channel : source.channels)
            {
                if (!channel.nodeIndex) continue;
                auto joint = std::find(ordered.begin(), ordered.end(), *channel.nodeIndex);
                if (joint == ordered.end()) continue;
                if (channel.path == fastgltf::AnimationPath::Weights) continue;

                const auto& sampler = source.samplers.at(channel.samplerIndex);
                if (sampler.interpolation != fastgltf::AnimationInterpolation::Linear)
                    throw std::runtime_error("Nonlinear glTF animation needs resampling");

                const auto& inputs = asset.accessors.at(sampler.inputAccessor);
                const auto& outputs = asset.accessors.at(sampler.outputAccessor);
                if (inputs.count != outputs.count)
                    throw std::runtime_error("Mismatched glTF animation channels");

                auto& track = clip.tracks[std::size_t(joint - ordered.begin())];
                for (std::size_t i = 0; i < inputs.count; ++i)
                {
                    const auto t = fastgltf::getAccessorElement<float>(asset, inputs, i);
                    duration = std::max(duration, t);
                    if (channel.path == fastgltf::AnimationPath::Translation)
                    {
                        const auto p = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                            asset, outputs, i);
                        track.translations.push_back({t, {p[0], p[1], p[2]}});
                    }
                    else if (channel.path == fastgltf::AnimationPath::Rotation)
                    {
                        const auto q = fastgltf::getAccessorElement<fastgltf::math::fvec4>(
                            asset, outputs, i);
                        track.rotations.push_back({t, {q[0], q[1], q[2], q[3]}});
                    }
                    else if (channel.path == fastgltf::AnimationPath::Scale)
                    {
                        const auto v = fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                            asset, outputs, i);
                        track.scales.push_back({t, {v[0], v[1], v[2]}});
                    }
                }
                has_channels = true;
            }

            if (!has_channels) continue;
            clip.duration = std::max(duration, 0.001f);
            for (std::size_t i = 0; i < ordered.size(); ++i)
            {
                auto& track = clip.tracks[i];
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
} // namespace srv::assets
