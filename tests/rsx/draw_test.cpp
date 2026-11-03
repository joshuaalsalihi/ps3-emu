// Drawing through the RSX into the reference backend, with the shaders cgcomp made for
// rsx_triangle: the vertex program passes position and color through, the fragment program
// outputs the color.

#include "support/draw_test.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <initializer_list>

using rsx::Rsx;
using test::DrawTest;
using test::f;
using test::k_depth;
using test::k_fragment_program;
using test::k_indices;
using test::k_square;
using test::k_vertices;
namespace method = rsx::method;

namespace {

constexpr std::array<float, 4> k_red = {1, 0, 0, 1};
constexpr u32 k_red_pixel = 0xffff'0000;

} // namespace

TEST_CASE_METHOD(DrawTest, "two triangles fill the surface") {
    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}, k_red);
    draw(method::k_triangles, 6);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(k_red_pixel) == 16 * 8);
    CHECK(pixel(16, 0) == 0); // the pixel past the surface's right edge
}

TEST_CASE_METHOD(DrawTest, "a triangle strip and a fan make the same square") {
    set_vertices({{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}, k_red);
    draw(method::k_triangle_strip, 4);
    CHECK(count(k_red_pixel) == 16 * 8);

    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, {0, 0, 1, 1});
    draw(method::k_triangle_fan, 4);
    CHECK(count(0xff00'00ff) == 16 * 8);
}

TEST_CASE_METHOD(DrawTest, "a triangle covers only its half") {
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    // The diagonal runs through pixel corners; the half below it (in clip space) is 8 per
    // column on the right, tapering to 0 on the left: 16 columns, 64 pixels.
    CHECK(count(k_red_pixel) == 64);
    CHECK(pixel(15, 0) == k_red_pixel);
    CHECK(pixel(0, 7) == 0);
}

TEST_CASE_METHOD(DrawTest, "colors are interpolated and rounded to 8 bits") {
    // Red at the left edge, black at the right: the pixel centered at x + 0.5 gets
    // 1 - (x + 0.5) / 16 of the red, times 255, rounded to nearest.
    u32 offset = k_vertices;
    const float vertices[][8] = {{-1, -1, 0, 1, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 0, 0, 1},
                                 {1, 1, 0, 1, 0, 0, 0, 1},   {-1, -1, 0, 1, 1, 0, 0, 1},
                                 {1, 1, 0, 1, 0, 0, 0, 1},   {-1, 1, 0, 1, 1, 0, 0, 1}};
    for (const auto& vertex : vertices) {
        for (const float value : vertex) {
            local(offset, f(value));
            offset += 4;
        }
    }
    draw(method::k_triangles, 6);
    CHECK(pixel(0, 3) == 0xfff7'0000);  // 255 * 0.96875 = 247.03 -> 247
    CHECK(pixel(15, 3) == 0xff08'0000); // 255 * 0.03125 = 7.97 -> 8
    CHECK(pixel(8, 0) == pixel(8, 7));  // red depends only on x
}

TEST_CASE_METHOD(DrawTest, "the color mask keeps channels") {
    memory.write<u32>(Rsx::k_local_base, 0x1234'5678);
    methods(method::k_color_mask, {0x0000'0100}); // green only
    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}, {1, 1, 1, 1});
    draw(method::k_triangles, 6);
    CHECK(pixel(0, 0) == 0x1234'ff78);
}

TEST_CASE_METHOD(DrawTest, "state we don't support yet is reported") {
    methods(method::k_stencil_test_enable, {1});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    REQUIRE(rsx.error());
    CHECK(*rsx.error() == "RSX: stencil testing is not supported yet");
}

TEST_CASE_METHOD(DrawTest, "vertices behind the viewer are reported") {
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red, 0);
    draw(method::k_triangles, 3);
    REQUIRE(rsx.error());
    CHECK(rsx.error()->find("w <= 0") != std::string::npos);
}

TEST_CASE_METHOD(DrawTest, "a fragment program in unmapped memory is an error") {
    methods(method::k_shader_program, {(k_fragment_program | 1) + 0x1000'0000}); // past local
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    CHECK(rsx.error());
}

TEST_CASE_METHOD(DrawTest, "an index array picks the vertices") {
    set_corners(k_red);
    set_square_indices();
    draw_indexed(method::k_index_16, 6);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(k_red_pixel) == 16 * 8);

    // 32-bit indices, starting at the second triangle's.
    set_corners({0, 0, 1, 1});
    for (u32 i = 0; i < 6; ++i) {
        memory.write<u32>(Rsx::k_local_base + k_indices + 4 * i, k_square[i]);
    }
    draw_indexed(method::k_index_32, 3, 3);
    CHECK(count(0xff00'00ff) == 64); // half the square
}

TEST_CASE_METHOD(DrawTest, "the depth test keeps the nearer surface, whatever the order") {
    enable_depth();
    set_square_indices();
    set_corners(k_red, 0.5f); // window z 0.75
    draw_indexed(method::k_index_16, 6);
    set_corners({0, 1, 0, 1}, -0.5f); // window z 0.25: in front
    draw_indexed(method::k_index_16, 6);
    set_corners({0, 0, 1, 1}, 0.0f); // window z 0.5: behind the green
    draw_indexed(method::k_index_16, 6);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(0xff00'ff00) == 16 * 8);
    // 0.25 * (2^24 - 1) = 4194303.75, rounded to nearest; the stencil byte is kept.
    CHECK(memory.read<u32>(Rsx::k_local_base + k_depth) == (4194304u << 8 | 0x00));

    // With writes off, a nearer surface draws but leaves the depth alone.
    methods(method::k_depth_write_enable, {0});
    set_corners({0, 0, 1, 1}, -0.9f);
    draw_indexed(method::k_index_16, 6);
    CHECK(count(0xff00'00ff) == 16 * 8);
    CHECK(memory.read<u32>(Rsx::k_local_base + k_depth) == (4194304u << 8));
}

TEST_CASE_METHOD(DrawTest, "GREATER passes only farther surfaces") {
    enable_depth(method::k_greater);
    for (u32 i = 0; i < 128 * 8; i += 4) {
        local(k_depth + i, 0x8000'0000); // depth 0.5
    }
    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}, k_red); // window z 0.5
    draw(method::k_triangles, 6);
    CHECK(count(k_red_pixel) == 0); // 0x800000 is not greater than itself
}

TEST_CASE_METHOD(DrawTest, "culling drops triangles by their winding on screen") {
    // This test's viewport doesn't flip y, so the clip-space triangle (-1, -1), (1, -1), (1, 1)
    // runs right then down on screen: clockwise.
    methods(method::k_cull_face_enable, {1});
    methods(method::k_cull_face, {method::k_cull_back});
    methods(method::k_front_face, {method::k_front_face_ccw});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(k_red_pixel) == 0);

    methods(method::k_front_face, {method::k_front_face_cw});
    draw(method::k_triangles, 3);
    CHECK(count(k_red_pixel) == 64);

    for (u32 i = 0; i < 128 * 8; i += 4) {
        local(i, 0);
    }
    methods(method::k_cull_face, {method::k_cull_front_and_back});
    draw(method::k_triangles, 3);
    CHECK(count(k_red_pixel) == 0);
}

TEST_CASE_METHOD(DrawTest, "a fragment program samples a texture through its unit's methods") {
    use_texture_programs();
    set_texture();
    set_textured_square();
    draw(method::k_triangles, 6);
    REQUIRE_FALSE(rsx.error());
    CHECK(pixel(7, 3) == 0xffff'0000);
    CHECK(pixel(8, 3) == 0xff00'00ff);
    CHECK(count(0xffff'0000) == 64);
}

TEST_CASE_METHOD(DrawTest, "sampling a disabled or unsupported texture is an error") {
    use_texture_programs();
    set_texture();
    methods(method::k_texture_control, {0});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    REQUIRE(rsx.error());
    CHECK(*rsx.error() == "RSX: the fragment program samples disabled texture unit 0");

    DrawTest other;
    other.use_texture_programs();
    other.set_texture(0x26); // DXT1
    other.set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    other.draw(method::k_triangles, 3);
    REQUIRE(other.rsx.error());
    CHECK(*other.rsx.error() == "RSX: texture unit 0: format 6 is not supported yet");
}

TEST_CASE_METHOD(DrawTest, "blending combines the fragment with the pixel") {
    for (u32 i = 0; i < 128 * 8; i += 4) {
        local(i, 0xff00'00ff); // blue, opaque
    }
    // Half-transparent red over it: source alpha, one minus source alpha; alpha keeps the
    // source's. Each channel is 0.5, which is 127.5, rounded to even: 128.
    methods(method::k_blend_func_source, {0x302 | 1 << 16});
    methods(method::k_blend_func_destination, {0x303 | 0 << 16});
    methods(method::k_blend_enable, {1});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}, {1, 0, 0, 0.5f});
    draw(method::k_triangles, 6);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(0x8080'0080) == 16 * 8);
}

TEST_CASE_METHOD(DrawTest, "blend equations") {
    const auto blended = [&](u32 equation, u32 stored, float source) {
        local(0, stored);
        methods(method::k_blend_func_source, {1 | 1 << 16}); // ONE
        methods(method::k_blend_func_destination, {1 | 1 << 16});
        methods(method::k_blend_equation, {equation | equation << 16});
        methods(method::k_blend_enable, {1});
        set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}},
                     {source, source, source, source});
        draw(method::k_triangles, 6);
        REQUIRE_FALSE(rsx.error());
        return pixel(0, 0);
    };
    // 200/255 + 0.25, clamped to 1; 200/255 - 0.25 is 136.25 steps; 0.25 - 200/255 clamps to 0.
    CHECK(blended(0x8006, 0xc8c8'c8c8, 0.25f) == 0xffff'ffff);
    CHECK(blended(0x800b, 0xc8c8'c8c8, 0.25f) == 0x8888'8888);
    CHECK(blended(0x800a, 0xc8c8'c8c8, 0.25f) == 0);
    // MIN and MAX ignore the factors.
    CHECK(blended(0x8007, 0x1020'30ff, 0.25f) == 0x1020'3040);
    CHECK(blended(0x8008, 0x1020'30ff, 0.25f) == 0x4040'40ff);
}

TEST_CASE_METHOD(DrawTest, "an unsupported blend factor is reported") {
    methods(method::k_blend_func_source, {0xf006});
    methods(method::k_blend_enable, {1});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    REQUIRE(rsx.error());
    CHECK(*rsx.error() == "RSX: blend factor 0xf006 is not supported yet");
}

TEST_CASE_METHOD(DrawTest, "the alpha test drops fragments before they write color or depth") {
    enable_depth();
    // Alpha 0.5 is 128 as an 8-bit value: not greater than 128, but greater or equal.
    methods(method::k_alpha_func, {method::k_greater, 0x80});
    methods(method::k_alpha_test_enable, {1});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, {1, 0, 0, 0.5f});
    draw(method::k_triangles, 3);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(0x80ff'0000) == 0);
    CHECK(memory.read<u32>(Rsx::k_local_base + k_depth + 128 * 7 + 4 * 15) == 0xffff'ff00);

    methods(method::k_alpha_func, {method::k_greater_equal});
    draw(method::k_triangles, 3);
    CHECK(count(0x80ff'0000) == 64);
    CHECK(memory.read<u32>(Rsx::k_local_base + k_depth + 128 * 7 + 4 * 15) == 0x8000'0000);
}

TEST_CASE_METHOD(DrawTest, "quads, quad strips and polygons cover their area") {
    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, k_red);
    draw(method::k_quads, 4);
    REQUIRE_FALSE(rsx.error());
    CHECK(count(k_red_pixel) == 16 * 8);

    // Two quads side by side, as a strip: corners alternate bottom and top.
    set_vertices({{-1, -1}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 1}}, {0, 0, 1, 1});
    draw(method::k_quad_strip, 6);
    CHECK(count(0xff00'00ff) == 16 * 8);

    set_vertices({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, {0, 1, 0, 1});
    draw(method::k_polygon, 4);
    CHECK(count(0xff00'ff00) == 16 * 8);
}
