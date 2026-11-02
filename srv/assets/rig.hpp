#pragma once

#include <fastgltf/types.hpp>
#include "srv/shared/scene/scene.hpp"

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace srv::assets
{
    // A validated Ozz skeleton and clips imported from one glTF skin.
    // GLB conversion happens once on the asset worker, not on gameplay ticks.
    struct rig
    {
        ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
        std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;
        std::vector<std::string> names;
        std::vector<std::uint32_t> skin_joints; // glTF skin order -> Ozz order
    };

    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index);
    ::scene::skin serialize_rig(const fastgltf::Asset& asset,
                                std::size_t skin_index, std::uint32_t id);
    ::scene::skin serialize_rig(
        const fastgltf::Asset& asset, std::size_t skin_index, std::uint32_t id, const rig& imported
    );
} // namespace srv::assets
