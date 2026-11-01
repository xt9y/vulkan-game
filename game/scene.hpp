#pragma once

#include "srv/shared/scene/scene.hpp"

namespace game::scene
{
    struct configuration
    {
        std::uint32_t floor_mesh;
        std::uint32_t cube_mesh;
        float floor_half_extent;
        float cube_half_extent;
        std::vector<::scene::instance> instances;
    };

    void create_start(::scene::state& data, const configuration& config);
} // namespace game::scene
