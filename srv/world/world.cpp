#include "world.hpp"

#include "gen.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace srv::world
{
    namespace 
    {
        bool less(const edit& a, const edit& b) 
        {
            return a.x < b.x || (a.x == b.x && a.z < b.z);
        }
    }; // namespace 
    
    void init(state& world, std::uint64_t seed)
    {
        world.seed = seed;
        world.edits.clear();
    }

    std::int32_t height(const state& world, std::int32_t x, std::int32_t z)
    {
        const auto it = std::lower_bound(world.edits.begin(), world.edits.end(), edit{x, z, 0}, less);

        if (it != world.edits.end() && it->x == x && it->z == z) 
        {
            return it->height;
        }

        return generator::height(world.seed, x, z);
    }

    void set_height(state &world, std::int32_t x, std::int32_t z, std::int32_t height) 
    {
        const auto it = std::lower_bound(world.edits.begin(), world.edits.end(), edit{x, z, 0}, less);

        const bool exists = it != world.edits.end() && it->x == x && it->z == z;

        if (height == generator::height(world.seed, x, z))
        {
            if (exists) 
            {
                world.edits.erase(it);
            }
        }
        else if (exists) 
        {
            it->height = height;
        }
        else 
        {
            world.edits.insert(it, {x, z, height});
        }
    }
    
    bool solid(const state &world, double x, double y, double z)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            x < std::numeric_limits<std::int32_t>::min() ||
            z < std::numeric_limits<std::int32_t>::min() ||
            x >= double(std::numeric_limits<std::int32_t>::max()) + 1.0 ||
            z >= double(std::numeric_limits<std::int32_t>::max()) + 1.0)
        {
            return true;
        }

        return y < height(world, std::int32_t(std::floor(x)), std::int32_t(std::floor(z)));
    }
    
    bool collides(const state& world, const aabb& box)
    {
        if (!std::isfinite(box.min_x) || !std::isfinite(box.min_y) ||
            !std::isfinite(box.min_z) || !std::isfinite(box.max_x) ||
            !std::isfinite(box.max_y) || !std::isfinite(box.max_z))
        {
            return true;
        }

        if (box.min_x >= box.max_x || 
            box.min_y >= box.max_y || 
            box.min_z >= box.max_z)
        {
            return false;
        }

        const auto x0 = std::floor(box.min_x);
        const auto z0 = std::floor(box.min_z);
        const auto x1 = std::ceil(box.max_x);
        const auto z1 = std::ceil(box.max_z);

        if (x0 < std::numeric_limits<std::int32_t>::min() ||
            z0 < std::numeric_limits<std::int32_t>::min() ||
            x1 > double(std::numeric_limits<std::int32_t>::max()) + 1.0 ||
            z1 > double(std::numeric_limits<std::int32_t>::max()) + 1.0)
        {
            return true;
        }

        for (auto z = std::int64_t(z0); z < std::int64_t(z1); ++z)
        {
            for (auto x = std::int64_t(x0); x < std::int64_t(x1); ++x)
            {
                if (box.min_y < height(world, std::int32_t(x), std::int32_t(z)))
                {
                    return true;
                }
            }
        }

        return false;
    }

    bool sample(const state& world, std::int32_t x, std::int32_t z, std::int32_t width, std::int32_t depth, std::int32_t* heights)
    {
        if (!heights || width <= 0 || depth <= 0 ||
            std::int64_t(x) + width - 1 > std::numeric_limits<std::int32_t>::max() ||
            std::int64_t(z) + depth - 1 > std::numeric_limits<std::int32_t>::max())
        {
            return false;
        }

        for (std::int32_t dz = 0; dz < depth; ++dz)
        {
            for (std::int32_t dx = 0; dx < width; ++dx)
            {
                heights[std::size_t(dz) * std::size_t(width) + std::size_t(dx)] = height(world, x + dx, z + dz);
            }
        }

        return true;
    }

}; // namespace srv::world
