#pragma once

#include "srv/shared/scene/material.hpp"

#include <fastgltf/types.hpp>
#include <cstdint>
#include <vector>

namespace srv::assets
{
    // Validate the source image on the server and transmit its original PNG/JPEG
    // bytes. The client decodes it to RGBA8 before uploading to the GPU.
    std::vector<::scene::texture> images(const fastgltf::Asset& asset,
                                          std::uint32_t first_texture);
} // namespace srv::assets
