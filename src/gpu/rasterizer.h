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

// Twice the triangle's signed area. Positive means clockwise on screen, as y grows downward.
s64 signed_area(const std::array<FixedPoint, 3>& vertices);

// Pixels are visited in 2x2 quads, so fragment programs can compare neighbors. Lane i of the quad
// whose top-left pixel is (x, y) is pixel (x + i % 2, y + i / 2).
struct Quad {
    u32 x = 0;       // even
    u32 y = 0;       // even
    u32 covered = 0; // bit i: lane i is inside the triangle and the area
    // Each lane's barycentric weights at its center, even outside the triangle: weight i
    // belongs to vertex i, and the three add up to `area`.
    std::array<std::array<s64, 3>, 4> weights{};
    s64 area = 0;
};

using QuadVisitor = std::function<void(const Quad& quad)>;

// Visits the quads with a covered pixel inside `area`, row by row. Triangles with no area cover
// nothing.
void rasterize(const std::array<FixedPoint, 3>& vertices, const rsx::Rect& area,
               const QuadVisitor& visit);

} // namespace gpu
