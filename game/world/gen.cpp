#include "gen.hpp"
#include "srv/game/options.hpp"
#include <cstdint>

namespace game::world::generator
{
    namespace 
    {
        std::uint64_t hash(std::uint64_t seed, std::int32_t x, std::int32_t z) 
        {
            std::uint64_t value = seed ^
                (std::uint64_t(std::uint32_t(x)) * 0x9e3779b185ebca87ULL) ^
                (std::uint64_t(std::uint32_t(z)) * 0xc2b2ae3d27d4eb4fULL);

            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ULL;
            value ^= value >> 27;
            value *= 0x94d049bb133111ebULL;

            return value ^ (value >> 31);
        }

        std::int32_t divide(std::int32_t value, std::int32_t size) 
        {
            const auto q = value / size;
            return q - (value % size < 0);
        }

        std::int32_t smooth(std::int32_t offset, std::int32_t size)
        {
            const auto t = offset * 256 / size;
            return t * t * (768 - 2 * t) / (256 * 256);
        }

        std::int32_t mix(std::int32_t a, std::int32_t b, std::int32_t t)
        {
            return a + (b - a) * t / 256;
        }

        std::int32_t noise(std::uint64_t seed, std::int32_t x, std::int32_t z, std::int32_t scale, std::int32_t amplitude) 
        {
            const auto cx = divide(x, scale);
            const auto cz = divide(z, scale);
            const auto tx = smooth(x - cx * scale, scale);
            const auto tz = smooth(z - cz * scale, scale);

            auto sample = [&](std::int32_t gx, std::int32_t gz)
            {
                return std::int32_t(hash(seed, gx, gz) % (2 * amplitude + 1)) - amplitude;
            };

            const auto a = mix(sample(cx, cz), sample(cx + 1, cz), tx);
            const auto b = mix(sample(cx, cz + 1), sample(cx + 1, cz + 1), tx);

            return mix(a, b, tz);
        }
    } // namespace

    std::int32_t height(std::uint64_t seed, std::int32_t x, std::int32_t z)
    {
        const auto& cfg = game::options::get()->simulation;
        return cfg.terrain_base + noise(seed, x, z, cfg.terrain_scale, cfg.terrain_amplitude) +
            noise(seed ^ 0xd1b54a32d192ed03ULL, x, z, cfg.detail_scale, cfg.detail_amplitude);
    }
} // namespace srv::world::generator
