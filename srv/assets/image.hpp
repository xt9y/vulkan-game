#pragma once

#include "srv/shared/scene/material.hpp"

#include <fastgltf/types.hpp>
#include <cstdint>
#include <vector>

namespace srv::assets
{
    // Decode on the server worker. The client only receives validated RGBA8
    // pixels and never parses an external glTF or image file.
    std::vector<::scene::texture> images(const fastgltf::Asset& asset,
                                          std::uint32_t first_texture);
} // namespace srv::assets
