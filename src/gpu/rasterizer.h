#pragma once

// Which pixels a triangle covers, decided exactly with integer arithmetic.
//
// Vertex positions are in fixed point with 4 fraction bits (1/16 pixel). A pixel is covered if
// its center is inside the triangle; a center exactly on an edge belongs to the triangle only if
// that edge is a top or left edge, so triangles sharing an edge never both cover a pixel.
// UNVERIFIED: the RSX's subpixel precision and fill rule; these follow the usual conventions of
// its generation.

#include "common/types.h"
#include "rsx/backend.h"

#include <array>
#include <functional>

namespace gpu {

constexpr s64 k_subpixel_bits = 4;
constexpr s64 k_subpixel_scale = s64{1} << k_subpixel_bits;

struct FixedPoint {
    s64 x = 0; // in 1/16 pixels; y grows downward
    s64 y = 0;
};

// Called for each covered pixel with the triangle's barycentric weights at its center: weight i
// belongs to vertex i, and the three add up to `area`.
using CoverageVisitor =
    std::function<void(u32 x, u32 y, const std::array<s64, 3>& weights, s64 area)>;

// Visits the covered pixels inside `area`, row by row. Triangles with no area cover nothing.
void rasterize(const std::array<FixedPoint, 3>& vertices, const rsx::Rect& area,
               const CoverageVisitor& visit);

} // namespace gpu
