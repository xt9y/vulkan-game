#pragma once

#include "util/math/vector.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

namespace spatial
{
    using vec3 = vk::util::math::vec3;

    struct bounds {
        vec3 min;
        vec3 max;
    };

    struct plane {
        vec3 normal;
        float offset;
    };

    inline bool overlaps(const bounds& a, const bounds& b)
    {
        return a.min.x <= b.max.x && a.max.x >= b.min.x &&
               a.min.y <= b.max.y && a.max.y >= b.min.y &&
               a.min.z <= b.max.z && a.max.z >= b.min.z;
    }

    inline bool visible(const bounds& b, const std::array<plane, 6>& planes)
    {
        for (const auto& p : planes)
        {
            const vec3 corner{
                p.normal.x >= 0 ? b.max.x : b.min.x,
                p.normal.y >= 0 ? b.max.y : b.min.y,
                p.normal.z >= 0 ? b.max.z : b.min.z
            };

            if (vk::util::math::dot(p.normal, corner) + p.offset < 0)
            {
                return false;
            }
        }

        return true;
    }

    class index
    {
        struct node {
            bounds box{};
            std::uint32_t first = 0;
            std::uint32_t count = 0;
            std::uint32_t left = 0;
            std::uint32_t right = 0;
        };

        std::vector<bounds> boxes;
        std::vector<std::uint32_t> order;
        std::vector<node> nodes;

        static float axis(vec3 p, int dimension)
        {
            return dimension == 0 ? p.x :
                   dimension == 1 ? p.y : p.z;
        }

        static bounds merge(bounds a, bounds b)
        {
            return {
                {
                    std::min(a.min.x, b.min.x),
                    std::min(a.min.y, b.min.y),
                    std::min(a.min.z, b.min.z)
                },
                {
                    std::max(a.max.x, b.max.x),
                    std::max(a.max.y, b.max.y),
                    std::max(a.max.z, b.max.z)
                }
            };
        }

        std::uint32_t build(std::uint32_t first, std::uint32_t count)
        {
            const auto id = static_cast<std::uint32_t>(nodes.size());
            nodes.emplace_back();

            bounds box = boxes[order[first]];

            for (std::uint32_t i = 1; i < count; ++i)
            {
                box = merge(box, boxes[order[first + i]]);
            }

            nodes[id].box = box;

            if (count <= 8)
            {
                nodes[id].first = first;
                nodes[id].count = count;
                return id;
            }

            const vec3 extent = box.max - box.min;

            const int dimension =
                extent.x >= extent.y && extent.x >= extent.z ? 0 :
                extent.y >= extent.z ? 1 : 2;

            const auto middle = first + count / 2;

            std::nth_element(
                order.begin() + first,
                order.begin() + middle,
                order.begin() + first + count,
                [&](std::uint32_t a, std::uint32_t b)
                {
                    const auto ca = boxes[a].min + boxes[a].max;
                    const auto cb = boxes[b].min + boxes[b].max;

                    return axis(ca, dimension) < axis(cb, dimension);
                }
            );

            const auto left = build(first, middle - first);
            const auto right = build(middle, first + count - middle);

            nodes[id].left = left;
            nodes[id].right = right;

            return id;
        }

        template<class Accept, class Visit>
        void traverse(
            std::uint32_t id,
            Accept& accept,
            Visit& visit
        ) const
        {
            const auto& n = nodes[id];

            if (!accept(n.box))
            {
                return;
            }

            if (n.count)
            {
                for (std::uint32_t i = 0; i < n.count; ++i)
                {
                    const auto object = order[n.first + i];

                    if (accept(boxes[object]))
                    {
                        visit(object);
                    }
                }

                return;
            }

            traverse(n.left, accept, visit);
            traverse(n.right, accept, visit);
        }

    public:
        void rebuild(std::vector<bounds> source)
        {
            boxes = std::move(source);

            order.resize(boxes.size());
            std::iota(order.begin(), order.end(), 0u);

            nodes.clear();
            nodes.reserve(boxes.size() * 2);

            if (!boxes.empty())
            {
                build(0, static_cast<std::uint32_t>(boxes.size()));
            }
        }

        template<class Visit>
        void query(const bounds& region, Visit&& visit) const
        {
            if (nodes.empty()) return;

            auto accept = [&](const bounds& b)
            {
                return overlaps(region, b);
            };

            traverse(0, accept, visit);
        }

        template<class Visit>
        void query_frustum(const std::array<plane, 6>& planes, Visit&& visit) const
        {
            if (nodes.empty()) return;

            auto accept = [&](const bounds& b)
            {
                return visible(b, planes);
            };

            traverse(0, accept, visit);
        }
    };
} // namespace spatial
