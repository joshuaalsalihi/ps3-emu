#pragma once

#include "support/rsx_test.h"

#include <array>
#include <bit>
#include <initializer_list>
#include <utility>

namespace test {

inline u32 f(float value) {
    return std::bit_cast<u32>(value);
}

constexpr u32 k_fragment_program = 0x8000; // local offsets
constexpr u32 k_vertices = 0x9000;
constexpr u32 k_indices = 0xa000;
constexpr u32 k_depth = 0x4000;
constexpr u32 k_texture = 0x6000;
constexpr u32 k_square[] = {0, 1, 2, 0, 2, 3}; // the corners set_corners writes, as triangles
constexpr u32 k_stride = 32;                   // x, y, z, w, then r, g, b, a

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
        // The control word libgcm writes with these programs: the result is R0.
        methods(method::k_shader_control, {0x0200'0440});

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

    // The surface's corners, counterclockwise in clip space, all of one color and depth.
    void set_corners(std::array<float, 4> color, float z = 0) {
        u32 offset = k_vertices;
        for (const auto& [x, y] :
             {std::pair{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}) {
            for (const float value : {x, y, z, 1.0f, color[0], color[1], color[2], color[3]}) {
                local(offset, f(value));
                offset += 4;
            }
        }
    }

    void set_square_indices() {
        for (u32 i = 0; i < 6; ++i) {
            memory.write<u16>(Rsx::k_local_base + k_indices + 2 * i, static_cast<u16>(k_square[i]));
        }
    }

    void draw_indexed(u32 type, u32 count, u32 first = 0) {
        methods(method::k_index_array_offset, {k_indices, type << 4}); // in local memory
        methods(method::k_begin_end, {method::k_triangles});
        methods(method::k_draw_index_array, {first | (count - 1) << 24});
        methods(method::k_begin_end, {0});
        rsx.run();
    }

    // A Z24S8 depth buffer at k_depth, cleared to the far plane, with the depth test on.
    void enable_depth(u32 compare = method::k_less) {
        methods(method::k_surface_format, {8 | 2 << 5});
        methods(method::k_set_zeta_context, {method::k_context_local});
        methods(method::k_surface_zeta_offset, {k_depth});
        methods(method::k_surface_pitch_zeta, {128});
        for (u32 i = 0; i < 128 * 8; i += 4) {
            local(k_depth + i, 0xffff'ff00);
        }
        methods(method::k_depth_func, {compare});
        methods(method::k_depth_write_enable, {1});
        methods(method::k_depth_test_enable, {1});
    }

    // Switches to a vertex program that passes the position and texture coordinate 0 through
    // (texture coordinates come from the color slot of each vertex), and cgcomp's
    // TEX result.color, fragment.texcoord[0], texture[0].
    void use_texture_programs() {
        methods(method::k_transform_program_load, {0});
        methods(method::k_transform_program,
                {0x401f9c6c, 0x0040000d, 0x8106c083, 0x6041ff80,   // MOV o[HPOS], v[OPOS]
                 0x401f9c6c, 0x0040080d, 0x8106c083, 0x6041ff9d}); // MOV o[TEX0], v[TEX0]
        const u32 tex[] = {0x9e011700, 0xc8011c9d, 0xc8000001, 0xc8003fe1};
        for (u32 i = 0; i < 4; ++i) {
            memory.write<u32>(Rsx::k_local_base + k_fragment_program + 4 * i, tex[i]);
        }
        methods(method::k_vertex_data_array_offset + 4 * 8, {k_vertices + 16});
        methods(method::k_vertex_data_array_format + 4 * 8,
                {method::k_vertex_float | 2 << 4 | k_stride << 8});
    }

    // A linear 2x1 texture in unit 0, red then blue, sampled nearest and clamped. The format
    // byte is GCM's: A8R8G8B8 | LIN by default.
    void set_texture(u32 format_byte = 0x25) {
        local(k_texture, 0xffff'0000);
        local(k_texture + 4, 0xff00'00ff);
        methods(method::k_texture_offset,
                {k_texture, 1 | 2 << 4 | format_byte << 8 | 1 << 16, // local, 2D, 1 level
                 0x0003'0303,                                        // clamp to edge
                 1u << 31,                                           // enabled
                 0xaae4,                                             // identity remap
                 1 << 16 | 1 << 24,                                  // nearest
                 2 << 16 | 1});                                      // 2x1
        methods(method::k_texture_size, {8});                        // pitch
    }

    // Two triangles covering the surface, with s running 0..1 across it and t at 0.5.
    void set_textured_square() {
        u32 offset = k_vertices;
        const float vertices[][8] = {{-1, -1, 0, 1, 0, 0.5f, 0, 0}, {1, -1, 0, 1, 1, 0.5f, 0, 0},
                                     {1, 1, 0, 1, 1, 0.5f, 0, 0},   {-1, -1, 0, 1, 0, 0.5f, 0, 0},
                                     {1, 1, 0, 1, 1, 0.5f, 0, 0},   {-1, 1, 0, 1, 0, 0.5f, 0, 0}};
        for (const auto& vertex : vertices) {
            for (const float value : vertex) {
                local(offset, f(value));
                offset += 4;
            }
        }
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
