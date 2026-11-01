#pragma once

#include <cstdint>
#include <vector>

namespace srv::world 
{
    struct edit {
        std::int32_t x;
        std::int32_t z;
        std::int32_t height;
    };

    using height_generator = std::int32_t (*)(std::uint64_t, std::int32_t, std::int32_t);

    struct state {
        std::uint64_t seed = 0;
        height_generator generator = nullptr;
        std::vector<edit> edits;
    };

    struct aabb {
        double min_x;
        double min_y;
        double min_z;
        double max_x;
        double max_y;
        double max_z;
    };

    void init(state& world, std::uint64_t seed, height_generator generator);
    
    std::int32_t height(const state& world, std::int32_t x, std::int32_t z);

    void set_height(state& world, std::int32_t x, std::int32_t z, std::int32_t height);

    bool solid(const state& world, double x, double y, double z);

    bool collides(const state& world, const aabb& box);

    bool sample(const state& world, std::int32_t x, std::int32_t z, std::int32_t width, std::int32_t depth, std::int32_t* heights);
} // namespace srv::world
