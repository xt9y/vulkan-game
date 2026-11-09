#pragma once
#include "scene.hpp"
#include <fastgltf/types.hpp>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace srv::assets {
std::vector<::scene::texture> images(const fastgltf::Asset& asset, std::uint32_t first_texture);

::scene::state load(const std::filesystem::path& path, std::uint32_t first_mesh, std::uint32_t first_instance, std::uint32_t first_texture, std::uint32_t first_skin);

struct rig {
        ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
        std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;
        std::vector<std::string> names;
        std::vector<std::uint32_t> skin_joints; // glTF skin order -> Ozz order
    };
    rig import_rig(const fastgltf::Asset& asset, std::size_t skin_index);
::scene::skin serialize_rig( const fastgltf::Asset& asset, std::size_t skin_index, std::uint32_t id, const rig& imported );
}
