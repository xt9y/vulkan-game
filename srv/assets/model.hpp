#pragma once

#include "srv/shared/scene/scene.hpp"

#include <cstdint>
#include <filesystem>

namespace srv::assets
{
    // CPU-only glTF import. The caller publishes the resulting scene on the
    // authoritative server thread; no renderer or Vulkan objects are involved.
    ::scene::state load(const std::filesystem::path& path,
                        std::uint32_t first_mesh,
                        std::uint32_t first_instance,
                        std::uint32_t first_texture,
                        std::uint32_t first_skin);
} // namespace srv::assets
