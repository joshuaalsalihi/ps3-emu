// Drawing through the RSX into the reference backend, with the shaders cgcomp made for
// rsx_triangle: the vertex program passes position and color through, the fragment program
// outputs the color.

#include "support/rsx_test.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <initializer_list>

using rsx::Rsx;
using test::RsxTest;
namespace method = rsx::method;

namespace {

u32 f(float value) {
    return std::bit_cast<u32>(value);
}

constexpr u32 k_fragment_program = 0x8000; // local offsets
constexpr u32 k_vertices = 0x9000;
constexpr u32 k_stride = 32; // x, y, z, w, then r, g, b, a

class DrawTest : public RsxTest {
public:
    DrawTest() {
        set_surface(8); // 16x8 A8R8G8B8
        // Clip space to pixels: x from -1..1 to 0..16, y from -1..1 to 0..8.
        methods(method::k_viewport_offset, {f(8), f(4), f(0.5f), f(0)});
        methods(method::k_viewport_scale, {f(8), f(4), f(0.5f), f(0)});

        methods(method::k_transform_program_load, {0});
        methods(method::k_transform_program,
                {0x401f9c6c, 0x0040000d, 0x8106c083, 0x6041ff80,   // MOV o[HPOS], v[OPOS]
                 0x401f9c6c, 0x0040030d, 0x8106c083, 0x6041ff85}); // MOV o[COL0], v[COL0]
        methods(method::k_transform_program_start, {0});

        const u32 fragment[] = {0x3e010100, 0xc8011c9d, 0xc8000001, 0xc8000001}; // MOV R0, COL0
        for (u32 i = 0; i < 4; ++i) {
            local(k_fragment_program + 4 * i, fragment[i]);
        }
        methods(method::k_shader_program, {k_fragment_program | 1}); // in local memory

        const u32 format = method::k_vertex_float | 4 << 4 | k_stride << 8;
        methods(method::k_vertex_data_array_offset, {k_vertices});
        methods(method::k_vertex_data_array_offset + 4 * 3, {k_vertices + 16});
        methods(method::k_vertex_data_array_format, {format});
        methods(method::k_vertex_data_array_format + 4 * 3, {format});
    }

    void local(u32 offset, u32 value) { memory.write<u32>(Rsx::k_local_base + offset, value); }

    // Writes vertices {x, y} (z = 0, w = `w`) all of one color.
    void set_vertices(std::initializer_list<std::array<float, 2>> positions,
                      std::array<float, 4> color, float w = 1) {
        u32 offset = k_vertices;
        for (const auto& [x, y] : positions) {
            for (const float value : {x, y, 0.0f, w, color[0], color[1], color[2], color[3]}) {
                local(offset, f(value));
                offset += 4;
            }
        }
    }

    void draw(u32 primitive, u32 count) {
        methods(method::k_begin_end, {primitive});
        methods(method::k_draw_arrays, {(count - 1) << 24});
        methods(method::k_begin_end, {0});
        rsx.run();
    }

    // Counts the 16x8 surface's pixels that hold `color`.
    int count(u32 color) const {
        int total = 0;
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                if (pixel(x, y) == color) ++total;
            }
        }
        return total;
    }
};

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
    methods(method::k_depth_test_enable, {1});
    set_vertices({{-1, -1}, {1, -1}, {1, 1}}, k_red);
    draw(method::k_triangles, 3);
    REQUIRE(rsx.error());
    CHECK(*rsx.error() == "RSX: depth testing is not supported yet");
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
