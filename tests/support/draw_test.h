#pragma once

#include "support/rsx_test.h"

#include <array>
#include <bit>
#include <initializer_list>

namespace test {

inline u32 f(float value) {
    return std::bit_cast<u32>(value);
}

constexpr u32 k_fragment_program = 0x8000; // local offsets
constexpr u32 k_vertices = 0x9000;
constexpr u32 k_stride = 32; // x, y, z, w, then r, g, b, a

// Draws with the shaders cgcomp made for rsx_triangle: the vertex program passes position and
// color through, the fragment program outputs the color. The surface is 16x8 A8R8G8B8.
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

} // namespace test
