#include "gpu/rasterizer.h"

#include <algorithm>

namespace gpu {

namespace {

// How far `p` is to one side of the edge from `a` to `b` (twice the signed area of a, b, p).
s64 edge(const FixedPoint& a, const FixedPoint& b, const FixedPoint& p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// With the vertices ordered so the area is positive (clockwise on screen, as y grows downward),
// a top edge runs exactly rightward and a left edge runs upward.
bool is_top_left(const FixedPoint& a, const FixedPoint& b) {
    return (a.y == b.y && b.x > a.x) || b.y < a.y;
}

s64 pixel_floor(s64 fixed) {
    return fixed >= 0 ? fixed / k_subpixel_scale
                      : -((-fixed + k_subpixel_scale - 1) / k_subpixel_scale);
}

} // namespace

void rasterize(const std::array<FixedPoint, 3>& vertices, const rsx::Rect& area,
               const CoverageVisitor& visit) {
    // Order the vertices so the area is positive, remembering where each came from.
    std::array<int, 3> order = {0, 1, 2};
    s64 total = edge(vertices[0], vertices[1], vertices[2]);
    if (total == 0) return;
    if (total < 0) {
        std::swap(order[1], order[2]);
        total = -total;
    }
    const FixedPoint& v0 = vertices[order[0]];
    const FixedPoint& v1 = vertices[order[1]];
    const FixedPoint& v2 = vertices[order[2]];

    // The bounding box in pixels, clipped to the area.
    const s64 min_x = std::max<s64>(area.x, pixel_floor(std::min({v0.x, v1.x, v2.x})));
    const s64 max_x =
        std::min<s64>(s64{area.x} + area.width - 1, pixel_floor(std::max({v0.x, v1.x, v2.x})));
    const s64 min_y = std::max<s64>(area.y, pixel_floor(std::min({v0.y, v1.y, v2.y})));
    const s64 max_y =
        std::min<s64>(s64{area.y} + area.height - 1, pixel_floor(std::max({v0.y, v1.y, v2.y})));

    // Each vertex's weight is the edge opposite it.
    const std::array<const FixedPoint*, 3> from = {&v1, &v2, &v0};
    const std::array<const FixedPoint*, 3> to = {&v2, &v0, &v1};
    std::array<bool, 3> top_left{};
    for (int i = 0; i < 3; ++i) {
        top_left[i] = is_top_left(*from[i], *to[i]);
    }

    for (s64 y = min_y; y <= max_y; ++y) {
        for (s64 x = min_x; x <= max_x; ++x) {
            const FixedPoint center{x * k_subpixel_scale + k_subpixel_scale / 2,
                                    y * k_subpixel_scale + k_subpixel_scale / 2};
            std::array<s64, 3> weights{};
            bool inside = true;
            for (int i = 0; i < 3 && inside; ++i) {
                weights[i] = edge(*from[i], *to[i], center);
                inside = weights[i] > 0 || (weights[i] == 0 && top_left[i]);
            }
            if (!inside) continue;

            std::array<s64, 3> original{};
            for (int i = 0; i < 3; ++i) {
                original[order[i]] = weights[i];
            }
            visit(static_cast<u32>(x), static_cast<u32>(y), original, total);
        }
    }
}

} // namespace gpu
