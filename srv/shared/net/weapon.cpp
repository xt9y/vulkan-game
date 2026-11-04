#include "weapon.hpp"

#include "movement.hpp"

namespace net::weapon
{
    namespace
    {
        using namespace net::movement::detail;

        bool
        header(const std::uint8_t* p, std::size_t size, std::size_t expected, std::uint8_t kind)
        {
            return p && size == expected && p[0] == 'V' && p[1] == 'G' && p[2] == 'U' &&
                   p[3] == 'N' && p[4] == 4 && p[5] == kind && !p[6] && !p[7];
        }

        void write_state(std::uint8_t* p, const snapshot& v)
        {
            put(p + 8, v.token);
            put(p + 12, v.sequence);
            put(p + 16, v.tick);
            put(p + 20, v.magazine);
            put(p + 24, v.reserve);
            put(p + 28, std::uint32_t(v.reload));
            put(p + 32, std::uint32_t(v.outcome));
            put_float(p + 36, v.reload_elapsed);
            put_float(p + 40, v.reload_duration);
            put_float(p + 44, v.refill_time);
            put(p + 48, v.weapon_id);
            put(p + 52, v.active_weapon_id);
        }

        std::optional<snapshot> read_state(const std::uint8_t* p)
        {
            snapshot v;
            v.token = get(p + 8);
            v.sequence = get(p + 12);
            v.tick = get(p + 16);
            v.magazine = get(p + 20);
            v.reserve = get(p + 24);
            v.reload = reload_phase(get(p + 28));
            v.outcome = result(get(p + 32));
            v.reload_elapsed = get_float(p + 36);
            v.reload_duration = get_float(p + 40);
            v.refill_time = get_float(p + 44);
            v.weapon_id = get(p + 48);
            v.active_weapon_id = get(p + 52);
            if (!v.token || !v.weapon_id || !v.active_weapon_id || get(p + 28) > 2 ||
                get(p + 32) > std::uint32_t(result::equipped) ||
                !std::isfinite(v.reload_elapsed) || !std::isfinite(v.reload_duration) ||
                !std::isfinite(v.refill_time) || v.reload_elapsed < 0 || v.reload_duration < 0 ||
                v.refill_time < 0 ||
                v.reload_elapsed > v.reload_duration || v.refill_time > v.reload_duration)
            {
                return std::nullopt;
            }
            return v;
        }
    } // namespace

    command_packet encode_command(const command& v)
    {
        command_packet p{'V', 'G', 'U', 'N', 4, 1, 0, 0};
        put(p.data() + 8, v.token);
        put(p.data() + 12, v.sequence);
        put(p.data() + 16, std::uint32_t(v.kind));
        put_float(p.data() + 20, v.yaw);
        put_float(p.data() + 24, v.pitch);
        put(p.data() + 28, v.weapon_id);
        return p;
    }

    std::optional<command> decode_command(const std::uint8_t* p, std::size_t size)
    {
        if (!header(p, size, command_packet{}.size(), 1))
        {
            return std::nullopt;
        }
        command v{
            get(p + 8), get(p + 12), action(get(p + 16)), get_float(p + 20), get_float(p + 24), get(p + 28)
        };
        if (!v.token || !v.sequence || !v.weapon_id || get(p + 16) < 1 || get(p + 16) > 3 ||
            !std::isfinite(v.yaw) || !std::isfinite(v.pitch) || std::abs(v.yaw) > 1000 ||
            std::abs(v.pitch) > 1.570797f)
        {
            return std::nullopt;
        }
        return v;
    }

    snapshot_packet encode_snapshot(const snapshot& v)
    {
        snapshot_packet p{'V', 'G', 'U', 'N', 4, 2, 0, 0};
        write_state(p.data(), v);
        return p;
    }

    std::optional<snapshot> decode_snapshot(const std::uint8_t* p, std::size_t size)
    {
        if (!header(p, size, snapshot_packet{}.size(), 2))
        {
            return std::nullopt;
        }
        return read_state(p);
    }

    shot_packet encode_shot(const shot& v)
    {
        shot_packet p{'V', 'G', 'U', 'N', 4, 3, 0, 0};
        write_state(p.data(), v.state);
        const auto& h = v.impact;
        put(p.data() + 56, h.impacted ? 1 : 0);
        put_float(p.data() + 60, h.position.x);
        put_float(p.data() + 64, h.position.y);
        put_float(p.data() + 68, h.position.z);
        put_float(p.data() + 72, h.normal.x);
        put_float(p.data() + 76, h.normal.y);
        put_float(p.data() + 80, h.normal.z);
        put(p.data() + 84, h.instance);
        put(p.data() + 88, h.triangle);
        put(p.data() + 92, std::uint32_t(h.type));
        return p;
    }

    std::optional<shot> decode_shot(const std::uint8_t* p, std::size_t size)
    {
        if (!header(p, size, shot_packet{}.size(), 3))
        {
            return std::nullopt;
        }
        const auto state = read_state(p);
        if (!state || get(p + 56) > 1 || get(p + 92) > std::uint32_t(::weapon::impact_type::glass))
        {
            return std::nullopt;
        }
        shot v{
            *state,
            {get(p + 56) == 1,
             {get_float(p + 60), get_float(p + 64), get_float(p + 68)},
             {get_float(p + 72), get_float(p + 76), get_float(p + 80)},
             get(p + 84),
             get(p + 88),
             ::weapon::impact_type(get(p + 92))}
        };
        const auto& h = v.impact;
        auto finite = [](float x)
        {
            return std::isfinite(x) && std::abs(x) < 1000000;
        };
        if (!finite(h.position.x) || !finite(h.position.y) || !finite(h.position.z) ||
            !finite(h.normal.x) || !finite(h.normal.y) || !finite(h.normal.z))
        {
            return std::nullopt;
        }
        if (h.impacted && (state->outcome != result::fired || !h.instance ||
                           std::abs(vk::util::math::length_squared(h.normal) - 1.0f) > 0.01f))
        {
            return std::nullopt;
        }
        return v;
    }
} // namespace net::weapon
