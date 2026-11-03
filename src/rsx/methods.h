#pragma once

// RSX method numbers: the registers a FIFO command writes. The RSX is an NVIDIA NV40-family GPU;
// its 3D class is 0x4097 (NV40's "curie"). Numbers: PSL1GHT's rsx/nv40.h (from nouveau's
// headers) and envytools' rnndb.

#include "common/types.h"

namespace rsx::method {

// Every class: bind an object to the subchannel.
constexpr u32 k_set_object = 0x0000;

// The FIFO channel's own methods, on any subchannel (class 0x406e).
constexpr u32 k_set_reference = 0x0050;         // writes the control register's REF
constexpr u32 k_set_semaphore_context = 0x0060; // which memory the semaphore offset is in
constexpr u32 k_semaphore_offset = 0x0064;      // byte offset of the semaphore (a label)
constexpr u32 k_semaphore_acquire = 0x0068;     // wait until the semaphore holds this value
constexpr u32 k_semaphore_release = 0x006c;     // write this value to the semaphore

// 3D class (0x4097).
constexpr u32 k_set_notify_context = 0x0180;
constexpr u32 k_set_color_context_b = 0x018c; // DMA_COLOR1
constexpr u32 k_set_color_context_a = 0x0194; // DMA_COLOR0
constexpr u32 k_set_zeta_context = 0x0198;
constexpr u32 k_set_color_context_c = 0x01b4;     // DMA_COLOR2
constexpr u32 k_set_color_context_d = 0x01b8;     // DMA_COLOR3
constexpr u32 k_surface_clip_horizontal = 0x0200; // x | width << 16
constexpr u32 k_surface_clip_vertical = 0x0204;   // y | height << 16
constexpr u32 k_surface_format = 0x0208;
constexpr u32 k_surface_pitch_a = 0x020c;
constexpr u32 k_surface_color_offset_a = 0x0210;
constexpr u32 k_surface_zeta_offset = 0x0214;
constexpr u32 k_surface_color_offset_b = 0x0218;
constexpr u32 k_surface_pitch_b = 0x021c;
constexpr u32 k_surface_color_target = 0x0220;
constexpr u32 k_surface_pitch_zeta = 0x022c;
constexpr u32 k_surface_pitch_c = 0x0280;
constexpr u32 k_surface_pitch_d = 0x0284;
constexpr u32 k_surface_color_offset_c = 0x0288;
constexpr u32 k_surface_color_offset_d = 0x028c;
constexpr u32 k_alpha_test_enable = 0x0304;
constexpr u32 k_alpha_func = 0x0308; // see k_never ... k_always
constexpr u32 k_alpha_ref = 0x030c;
constexpr u32 k_blend_enable = 0x0310;
constexpr u32 k_blend_func_source = 0x0314;      // RGB factor | alpha factor << 16
constexpr u32 k_blend_func_destination = 0x0318; // RGB factor | alpha factor << 16
constexpr u32 k_blend_color = 0x031c;            // A8R8G8B8
constexpr u32 k_blend_equation = 0x0320;         // RGB equation | alpha equation << 16
constexpr u32 k_color_mask = 0x0324;             // A, R, G, B enables: bits 24, 16, 8, 0
constexpr u32 k_stencil_test_enable = 0x0348;
constexpr u32 k_shade_mode = 0x0368; // see k_shade_*
constexpr u32 k_logic_op_enable = 0x0374;
constexpr u32 k_scissor_horizontal = 0x08c0; // x | width << 16
constexpr u32 k_scissor_vertical = 0x08c4;   // y | height << 16
constexpr u32 k_shader_program = 0x08e4;     // fragment program offset | location (1 local, 2 main)
constexpr u32 k_viewport_offset = 0x0a20;    // 4 floats: x, y, z, w
constexpr u32 k_viewport_scale = 0x0a30;     // 4 floats
constexpr u32 k_depth_func = 0x0a6c;         // see k_never ... k_always
constexpr u32 k_depth_write_enable = 0x0a70;
constexpr u32 k_depth_test_enable = 0x0a74;
constexpr u32 k_transform_program = 0x0b80;        // 32 words: the next words of the vertex program
constexpr u32 k_vertex_data_array_offset = 0x1680; // 16 of: offset | location << 31 (1 main)
constexpr u32 k_vertex_data_array_format = 0x1740; // 16 of: type | size << 4 | stride << 8
constexpr u32 k_begin_end = 0x1808;                // a primitive type, or 0 to end
constexpr u32 k_draw_arrays = 0x1814;              // first | (count - 1) << 24
constexpr u32 k_index_array_offset = 0x181c;
constexpr u32 k_index_array_format = 0x1820; // location (0 local, 1 main) | type << 4
constexpr u32 k_draw_index_array = 0x1824;   // first | (count - 1) << 24
constexpr u32 k_cull_face = 0x1830;          // see k_cull_*
constexpr u32 k_front_face = 0x1834;         // see k_front_face_*
constexpr u32 k_cull_face_enable = 0x183c;
constexpr u32 k_texture_size = 0x1840; // 16 of: pitch | depth << 20
// The fragment textures, 16 units of 8 methods each, 0x20 apart.
constexpr u32 k_texture_offset = 0x1a00;
constexpr u32 k_texture_format = 0x1a04;  // see Texture in rsx/backend.h
constexpr u32 k_texture_address = 0x1a08; // wrap modes
constexpr u32 k_texture_control = 0x1a0c; // enable, LOD range
constexpr u32 k_texture_remap = 0x1a10;   // channel order
constexpr u32 k_texture_filter = 0x1a14;
constexpr u32 k_texture_image_rect = 0x1a18; // height | width << 16
constexpr u32 k_texture_border_color = 0x1a1c;
constexpr u32 k_texture_stride = 0x20;
constexpr u32 k_shader_control = 0x1d60;      // the fragment program's control word (FP_CONTROL)
constexpr u32 k_semaphore_offset_3d = 0x1d6c; // the 3D class's own label writes
constexpr u32 k_back_end_write_semaphore_release = 0x1d70;
constexpr u32 k_texture_read_semaphore_release = 0x1d74;
constexpr u32 k_zstencil_clear_value = 0x1d8c;    // Z16, or Z24 << 8 | S8
constexpr u32 k_color_clear_value = 0x1d90;       // A8R8G8B8
constexpr u32 k_clear_surface = 0x1d94;           // which buffers to clear: see k_clear_*
constexpr u32 k_transform_program_load = 0x1e9c;  // where the next vertex program words go
constexpr u32 k_transform_program_start = 0x1ea0; // where the vertex program starts
constexpr u32 k_transform_constant_load = 0x1efc; // where the next constants go
constexpr u32 k_transform_constant = 0x1f00;      // 32 words: the next constants' components

// Comparison functions, for depth and alpha tests. PSL1GHT, rsx/gcm_sys.h: GCM_NEVER ...
constexpr u32 k_never = 0x0200;
constexpr u32 k_less = 0x0201;
constexpr u32 k_equal = 0x0202;
constexpr u32 k_less_equal = 0x0203;
constexpr u32 k_greater = 0x0204;
constexpr u32 k_not_equal = 0x0205;
constexpr u32 k_greater_equal = 0x0206;
constexpr u32 k_always = 0x0207;

// k_cull_face and k_front_face values. GCM_CULL_*, GCM_FRONTFACE_*.
constexpr u32 k_cull_front = 0x0404;
constexpr u32 k_cull_back = 0x0405;
constexpr u32 k_cull_front_and_back = 0x0408;
constexpr u32 k_front_face_cw = 0x0900;
constexpr u32 k_front_face_ccw = 0x0901;

// k_index_array_format types. GCM_INDEX_TYPE_*.
constexpr u32 k_index_32 = 0;
constexpr u32 k_index_16 = 1;

// k_shade_mode values.
constexpr u32 k_shade_flat = 0x1d00;
constexpr u32 k_shade_smooth = 0x1d01;

// Vertex attribute types (k_vertex_data_array_format bits 0-3).
constexpr u32 k_vertex_float = 2;
constexpr u32 k_vertex_ubyte = 4; // unsigned bytes, normalized to 0..1

// k_begin_end values. PSL1GHT, rsx/gcm_sys.h: GCM_TYPE_*.
constexpr u32 k_triangles = 5;
constexpr u32 k_triangle_strip = 6;
constexpr u32 k_triangle_fan = 7;
constexpr u32 k_quads = 8;
constexpr u32 k_quad_strip = 9;
constexpr u32 k_polygon = 10;

// k_clear_surface bits. PSL1GHT, rsx/gcm_sys.h: GCM_CLEAR_*.
constexpr u32 k_clear_depth = 0x01;
constexpr u32 k_clear_stencil = 0x02;
constexpr u32 k_clear_red = 0x10;
constexpr u32 k_clear_green = 0x20;
constexpr u32 k_clear_blue = 0x40;
constexpr u32 k_clear_alpha = 0x80;

// libgcm binds the same object to each subchannel, so a method's meaning follows from its
// subchannel. UNVERIFIED: the binding, which our HLE libgcm doesn't write; seen in the FIFO.
constexpr u32 k_subchannel_3d = 0;
constexpr u32 k_subchannel_surfaces_2d = 3;    // NV04_CONTEXT_SURFACES_2D: where transfers go
constexpr u32 k_subchannel_image_from_cpu = 5; // NV40_IMAGE_FROM_CPU: data in the FIFO

// 2D surfaces (nouveau: NV04_CONTEXT_SURFACES_2D_*).
constexpr u32 k_surfaces_2d_destination_context = 0x0188;
constexpr u32 k_surfaces_2d_format = 0x0300; // see k_surface_2d_y32
constexpr u32 k_surfaces_2d_pitch = 0x0304;  // source | destination << 16
constexpr u32 k_surfaces_2d_destination_offset = 0x030c;
constexpr u32 k_surface_2d_y32 = 0x0b; // one 32-bit value per pixel

// Image from CPU (nouveau: NV01_IMAGE_FROM_CPU_*): pixels written into the 2D surface, row by
// row, from the words of the COLOR methods.
constexpr u32 k_image_point = 0x0304;    // x | y << 16
constexpr u32 k_image_size_out = 0x0308; // width | height << 16: the pixels that are written
constexpr u32 k_image_size_in = 0x030c;  // width | height << 16: the pixels that are sent
constexpr u32 k_image_color = 0x0400;    // up to 0x700 bytes of methods: the next pixels

// Methods only our HLE libgcm writes, on a subchannel guest code doesn't use. UNVERIFIED: real
// libgcm drives flips through the RSX's driver methods, whose encoding we don't know yet.
constexpr u32 k_hle_subchannel = 7;
constexpr u32 k_hle_flip = 0x0100;      // queue a flip to display buffer N
constexpr u32 k_hle_wait_flip = 0x0104; // wait until no flip is queued

// DMA context handles libgcm uses for the RSX's own memory and for main memory. PSL1GHT,
// rsx/gcm_sys.h: GCM_DMA_MEMORY_FRAME_BUFFER and GCM_DMA_MEMORY_HOST_BUFFER.
constexpr u32 k_context_local = 0xfeed'0000;
constexpr u32 k_context_main = 0xfeed'0001;

} // namespace rsx::method
