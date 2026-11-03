// Triangle coverage. Coordinates are in 1/16 pixels, so pixel (x, y) has its center at
// (16x + 8, 16y + 8).

#include "gpu/rasterizer.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <utility>
#include <vector>

using gpu::FixedPoint;

namespace {

constexpr rsx::Rect k_everything = {0, 0, 64, 64};

std::vector<std::pair<u32, u32>> covered(const std::array<FixedPoint, 3>& triangle,
                                         const rsx::Rect& area = k_everything) {
    std::vector<std::pair<u32, u32>> pixels;
    gpu::rasterize(triangle, area, [&](const gpu::Quad& quad) {
        for (u32 lane = 0; lane < 4; ++lane) {
            if (quad.covered & (1u << lane))
                pixels.emplace_back(quad.x + lane % 2, quad.y + lane / 2);
        }
    });
    return pixels;
}

FixedPoint at(s64 x, s64 y) {
    return {x, y};
}

} // namespace

TEST_CASE("triangles sharing an edge cover each pixel exactly once") {
    // A square split along either diagonal, with corners on pixel centers and off them, and
    // both windings: every pixel inside is covered once.
    for (const s64 offset : {0, 8, 3}) {
        const FixedPoint a = at(offset, offset);
        const FixedPoint b = at(offset + 128, offset);
        const FixedPoint c = at(offset + 128, offset + 128);
        const FixedPoint d = at(offset, offset + 128);
        for (const auto& halves : {std::pair{std::array{a, b, c}, std::array{a, c, d}},
                                   std::pair{std::array{b, d, a}, std::array{d, b, c}},
                                   std::pair{std::array{a, c, b}, std::array{d, c, a}}}) {
            std::map<std::pair<u32, u32>, int> count;
            for (const auto& pixel : covered(halves.first))
                ++count[pixel];
            for (const auto& pixel : covered(halves.second))
                ++count[pixel];
            INFO("offset " << offset);
            CHECK(count.size() == 64); // an 8x8 square
            for (const auto& [pixel, times] : count) {
                CHECK(times == 1);
            }
        }
    }
}

TEST_CASE("a center on a left or top edge is inside, on a right or bottom edge outside") {
    // A rectangle from x = 8 to 40 and y = 8 to 40 (centers of pixels 0 and 2) as two halves:
    // pixels 0 and 1 in each direction are in; 2 is out.
    std::map<std::pair<u32, u32>, int> count;
    for (const auto& pixel : covered({at(8, 8), at(40, 8), at(40, 40)}))
        ++count[pixel];
    for (const auto& pixel : covered({at(8, 8), at(40, 40), at(8, 40)}))
        ++count[pixel];
    CHECK(count.size() == 4);
    CHECK(count.contains({0, 0}));
    CHECK(count.contains({1, 1}));
    CHECK_FALSE(count.contains({2, 0}));
    CHECK_FALSE(count.contains({0, 2}));
}

TEST_CASE("coverage stays inside the area") {
    const auto pixels = covered({at(0, 0), at(256, 0), at(0, 256)}, {2, 3, 4, 1});
    CHECK(pixels.size() == 4);
    for (const auto& [x, y] : pixels) {
        CHECK(x >= 2);
        CHECK(x < 6);
        CHECK(y == 3);
    }
}

TEST_CASE("a triangle with no area covers nothing") {
    CHECK(covered({at(0, 0), at(64, 64), at(128, 128)}).empty());
}

TEST_CASE("the weights are each vertex's share of the area") {
    gpu::rasterize({at(0, 0), at(64, 0), at(0, 64)}, k_everything, [](const gpu::Quad& quad) {
        for (const auto& weights : quad.weights) {
            CHECK(weights[0] + weights[1] + weights[2] == quad.area);
        }
        if (quad.x == 0 && quad.y == 0) {
            // Lane 0's center (8, 8): vertex 1's share is 8 * 64, as is vertex 2's.
            CHECK(quad.weights[0][1] == 8 * 64);
            CHECK(quad.weights[0][2] == 8 * 64);
        }
    });
}

TEST_CASE("quads are aligned and hold helper lanes outside the triangle") {
    // A triangle covering only pixel (1, 1): its quad is the one at (0, 0), with lane 3 covered.
    std::vector<gpu::Quad> quads;
    gpu::rasterize({at(16, 16), at(48, 16), at(16, 48)}, k_everything,
                   [&](const gpu::Quad& quad) { quads.push_back(quad); });
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].x == 0);
    CHECK(quads[0].y == 0);
    CHECK(quads[0].covered == 0b1000);
    // Lane 0, the pixel up and to the left, lies outside: one of its weights is negative.
    const auto& outside = quads[0].weights[0];
    CHECK((outside[0] < 0 || outside[1] < 0 || outside[2] < 0));
}

TEST_CASE("the signed area is positive for triangles clockwise on screen") {
    // y grows downward, so right then down is clockwise.
    CHECK(gpu::signed_area({at(0, 0), at(16, 0), at(0, 16)}) > 0);
    CHECK(gpu::signed_area({at(0, 0), at(0, 16), at(16, 0)}) < 0);
}
