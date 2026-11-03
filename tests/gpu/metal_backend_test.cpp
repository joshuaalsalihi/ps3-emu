// The Metal backend against the reference backend: the same draws must give the same pixels,
// give or take the GPU's own rounding. macOS only.

#include "gpu/metal_backend.h"

#include "support/draw_test.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <functional>

using test::DrawTest;
namespace method = rsx::method;

namespace {

// Runs `scene` once with each backend and counts the pixels that differ by more than one step
// in any channel.
int differing_pixels(const std::function<void(DrawTest&)>& scene) {
    DrawTest reference;
    scene(reference);
    REQUIRE_FALSE(reference.rsx.error());

    DrawTest metal;
    std::string error;
    auto backend = gpu::MetalBackend::create(metal.memory, error);
    if (!backend) SKIP(error);
    metal.rsx.set_backend(backend.get());
    scene(metal);
    REQUIRE_FALSE(metal.rsx.error());

    int count = 0;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 a = reference.pixel(x, y);
            const u32 b = metal.pixel(x, y);
            for (int shift = 0; shift < 32; shift += 8) {
                if (std::abs(int((a >> shift) & 0xff) - int((b >> shift) & 0xff)) > 1) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("Metal fills a triangle like the reference") {
    CHECK(differing_pixels([](DrawTest& test) {
              test.set_vertices({{-1, -1}, {1, -1}, {1, 1}}, {1, 0, 0, 1});
              test.draw(method::k_triangles, 3);
          }) == 0);
}

TEST_CASE("Metal interpolates colors like the reference") {
    CHECK(differing_pixels([](DrawTest& test) {
              u32 offset = test::k_vertices;
              const float vertices[][8] = {
                  {-1, -1, 0, 1, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 0, 0, 1, 1}};
              for (const auto& vertex : vertices) {
                  for (const float value : vertex) {
                      test.local(offset, test::f(value));
                      offset += 4;
                  }
              }
              test.draw(method::k_triangles, 3);
          }) == 0);
}

TEST_CASE("Metal keeps the color mask and scissor") {
    CHECK(differing_pixels([](DrawTest& test) {
              test.memory.write<u32>(rsx::Rsx::k_local_base, 0x1234'5678);
              test.methods(method::k_color_mask, {0x0000'0100});
              test.methods(method::k_scissor_horizontal, {2 | 5u << 16});
              test.set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}},
                                {1, 1, 1, 1});
              test.draw(method::k_triangles, 6);
          }) == 0);
}

TEST_CASE("Metal samples a texture like the reference") {
    CHECK(differing_pixels([](DrawTest& test) {
              test.use_texture_programs();
              test.set_texture();
              test.set_textured_square();
              test.draw(method::k_triangles, 6);
          }) == 0);
}

TEST_CASE("Metal filters a texture like the reference") {
    CHECK(differing_pixels([](DrawTest& test) {
              test.use_texture_programs();
              test.set_texture();
              test.methods(method::k_texture_filter, {2 << 16 | 2 << 24}); // linear
              test.set_textured_square();
              test.draw(method::k_triangles, 6);
          }) == 0);
}

TEST_CASE("Metal depth-tests like the reference") {
    CHECK(differing_pixels([](DrawTest& test) {
              test.enable_depth();
              test.set_square_indices();
              test.set_corners({1, 0, 0, 1}, 0.5f);
              test.draw_indexed(method::k_index_16, 6);
              test.set_corners({0, 1, 0, 1}, -0.5f);
              test.draw_indexed(method::k_index_16, 6);
              test.set_corners({0, 0, 1, 1}, 0.0f);
              test.draw_indexed(method::k_index_16, 6);
              // And a triangle cutting through the green square, so depth varies per pixel.
              u32 offset = test::k_vertices;
              const float vertices[][8] = {
                  {-1, -1, -1, 1, 1, 1, 0, 1}, {1, -1, 1, 1, 1, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}};
              for (const auto& vertex : vertices) {
                  for (const float value : vertex) {
                      test.local(offset, test::f(value));
                      offset += 4;
                  }
              }
              test.draw(method::k_triangles, 3);
          }) == 0);
}

TEST_CASE("Metal culls like the reference") {
    for (const u32 front : {method::k_front_face_ccw, method::k_front_face_cw}) {
        for (const u32 cull : {method::k_cull_back, method::k_cull_front}) {
            CHECK(differing_pixels([&](DrawTest& test) {
                      test.methods(method::k_cull_face_enable, {1});
                      test.methods(method::k_cull_face, {cull});
                      test.methods(method::k_front_face, {front});
                      // One triangle of each winding.
                      test.set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {-1, 1}, {1, 1}},
                                        {1, 0, 0, 1});
                      test.draw(method::k_triangles, 6);
                  }) == 0);
        }
    }
}
