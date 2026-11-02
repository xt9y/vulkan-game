#pragma once

#include "weapon.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

namespace weapon
{
    // Consume the conversion sidecar rather than estimating animation length from gameplay time.
    // The sidecar supplies per-clip durations, frame rates and source frame bounds.
    inline timing load_timing(
        const std::filesystem::path& path,
        const std::string& reload_clip,
        const std::string& empty_reload_clip,
        float refill_frame,
        float empty_refill_frame
    )
    {
        std::ifstream input(path);
        if (!input)
        {
            throw std::runtime_error("Weapon animation metadata missing: " + path.string());
        }
        const std::string json((std::istreambuf_iterator<char>(input)), {});
        auto clip = [&](const std::string& name, float event_frame)
        {
            std::string escaped;
            for (const char c : name)
            {
                if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos)
                {
                    escaped += '\\';
                }
                escaped += c;
            }
            const std::regex pattern("\\{[^{}]*\"name\"\\s*:\\s*\"" + escaped + "\"[^{}]*\\}");
            std::smatch match;
            if (!std::regex_search(json, match, pattern))
            {
                throw std::runtime_error("Weapon animation metadata missing clip " + name);
            }
            const std::string object = match.str();
            auto number = [&](const std::string& key)
            {
                const std::regex field(
                    "\"" + key + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"
                );
                std::smatch value;
                if (!std::regex_search(object, value, field))
                {
                    throw std::runtime_error("Weapon animation metadata missing " + key);
                }
                return std::stof(value[1].str());
            };
            const float duration = number("duration_seconds");
            const float fps = number("fps");
            if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(duration) || duration <= 0 ||
                !std::isfinite(event_frame))
            {
                throw std::runtime_error("Invalid weapon animation timing");
            }
            // The source clip start is rebased to time zero by the converter.
            return std::pair{
                duration, std::max(0.0f, event_frame - number("source_first_frame")) / fps
            };
        };
        const auto normal = clip(reload_clip, refill_frame),
                   empty = clip(empty_reload_clip, empty_refill_frame);
        return {normal.first, empty.first, normal.second, empty.second};
    }
} // namespace weapon
