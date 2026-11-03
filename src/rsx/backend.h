#pragma once

// What the RSX's state model asks a GPU backend to do. The state model decodes registers into
// these requests; a backend only renders. Render targets live in guest memory, so a backend that
// renders elsewhere (the GPU) must write its results back before anyone reads them.

#include "common/types.h"
#include "rsx/fragment_program.h"
#include "rsx/vertex_program.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace rsx {

constexpr u32 k_texture_units = 16;

// Color surface formats (the low 5 bits of SURFACE_FORMAT). PSL1GHT, rsx/gcm_sys.h:
// GCM_SURFACE_*.
enum class ColorFormat : u32 {
    x8r8g8b8_z8r8g8b8 = 4, // the X byte reads as 0
    x8r8g8b8_o8r8g8b8 = 5, // the X byte reads as 1s
    a8r8g8b8 = 8,
};

// Depth formats (bits 5-7 of SURFACE_FORMAT). GCM_SURFACE_ZETA_*.
enum class DepthFormat : u32 { z16 = 1, z24s8 = 2 };

// A rectangle in pixels.
struct Rect {
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

struct ColorSurface {
    u32 address = 0; // guest address of pixel (0, 0)
    u32 pitch = 0;   // bytes per row
    ColorFormat format = ColorFormat::a8r8g8b8;
};

struct DepthSurface {
    u32 address = 0;
    u32 pitch = 0;
    DepthFormat format = DepthFormat::z24s8;
};

// Clears part of the render targets. Only the channels in `mask` change (method::k_clear_*).
struct ClearRequest {
    std::vector<ColorSurface> colors;
    std::optional<DepthSurface> depth;
    Rect area;
    u32 mask = 0;
    u32 color = 0;       // A8R8G8B8
    u32 depth_value = 0; // Z16 in the low 16 bits, or Z24 << 8 | S8
};

// A vertex array: where one input attribute of each vertex comes from.
struct VertexInput {
    u32 address = 0; // guest address of the first vertex's attribute
    u32 type = 0;    // method::k_vertex_float or k_vertex_ubyte
    u32 size = 0;    // components, 1-4; the rest default to 0, 0, 0, 1
    u32 stride = 0;  // bytes from one vertex to the next
};

enum class Primitive { triangles, triangle_strip, triangle_fan, quads, quad_strip, polygon };

// Comparison functions for the depth test (method::k_never ... k_always).
enum class Compare : u32 {
    never = 0x200,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

struct DepthTest {
    DepthSurface surface;
    Compare compare = Compare::less;
    bool write = true;
};

enum class Cull { none, front, back, front_and_back };

// Drops fragments whose alpha, as an 8-bit value, fails the comparison with `reference`.
struct AlphaTest {
    Compare compare = Compare::always;
    u32 reference = 0; // 0..255
};

// Blend factors and equations, as OpenGL defines them. PSL1GHT, rsx/gcm_sys.h: GCM_ZERO ...,
// GCM_FUNC_ADD ...
enum class BlendFactor : u32 {
    zero = 0,
    one = 1,
    source_color = 0x300,
    one_minus_source_color = 0x301,
    source_alpha = 0x302,
    one_minus_source_alpha = 0x303,
    destination_alpha = 0x304,
    one_minus_destination_alpha = 0x305,
    destination_color = 0x306,
    one_minus_destination_color = 0x307,
    source_alpha_saturate = 0x308,
    constant_color = 0x8001,
    one_minus_constant_color = 0x8002,
    constant_alpha = 0x8003,
    one_minus_constant_alpha = 0x8004,
};

enum class BlendEquation : u32 {
    add = 0x8006,
    minimum = 0x8007,
    maximum = 0x8008,
    subtract = 0x800a,         // source - destination
    reverse_subtract = 0x800b, // destination - source
};

// Combines each fragment's color (the source) with the pixel's (the destination):
// equation(source * source factor, destination * destination factor), per channel.
struct Blend {
    BlendFactor source_rgb = BlendFactor::one;
    BlendFactor source_alpha = BlendFactor::one;
    BlendFactor destination_rgb = BlendFactor::zero;
    BlendFactor destination_alpha = BlendFactor::zero;
    BlendEquation equation_rgb = BlendEquation::add;
    BlendEquation equation_alpha = BlendEquation::add;
    u32 color = 0; // the constant color, A8R8G8B8
};

// Texture formats we decode: the low 5 bits of the format byte. PSL1GHT, rsx/gcm_sys.h:
// GCM_TEXTURE_FORMAT_*. The D formats have no alpha channel: alpha reads as 1. B8 and G8B8 have
// only the channels they name; UNVERIFIED: the others read as 0 (programs remap them anyway).
enum class TextureFormat : u32 {
    b8 = 1,
    a1r5g5b5 = 2,
    a4r4g4b4 = 3,
    r5g6b5 = 4,
    a8r8g8b8 = 5,
    g8b8 = 11,
    d1r5g5b5 = 29,
    d8r8g8b8 = 30,
};

// What happens to coordinates outside 0..1. GCM_TEXTURE_REPEAT ...
enum class TextureWrap : u32 {
    repeat = 1,
    mirrored_repeat = 2,
    clamp_to_edge = 3,
    border = 4,
};

// GCM_TEXTURE_NEAREST ... The mipmap filters choose between levels as well as within one.
enum class TextureFilter : u32 {
    nearest = 1,
    linear = 2,
    nearest_mipmap_nearest = 3,
    linear_mipmap_nearest = 4,
    nearest_mipmap_linear = 5,
    linear_mipmap_linear = 6,
};

// A 2D texture a fragment program samples. Its mipmap levels follow level 0 in memory, each half
// the size of the one before (but at least 1x1).
struct Texture {
    u32 address = 0; // guest address of level 0's first texel
    TextureFormat format = TextureFormat::a8r8g8b8;
    bool linear = false;    // rows `pitch` bytes apart; otherwise swizzled (see gpu/texture.h)
    bool normalized = true; // coordinates 0..1 span the texture; otherwise they count texels
    u32 width = 1;
    u32 height = 1;
    u32 pitch = 0;
    u32 levels = 1;
    u32 remap = 0; // which channel each output reads (the low 16 bits of method::k_texture_remap)
    TextureWrap wrap_s = TextureWrap::repeat;
    TextureWrap wrap_t = TextureWrap::repeat;
    TextureFilter minify = TextureFilter::nearest;
    TextureFilter magnify = TextureFilter::nearest;
    u32 border_color = 0; // A8R8G8B8
    // The level of detail's range and bias, in 1/256 levels.
    u32 min_lod = 0;
    u32 max_lod = 0;
    s32 lod_bias = 0;
};

// Draws primitives into one color surface.
struct DrawRequest {
    Primitive primitive = Primitive::triangles;
    std::vector<u32> vertices; // vertex indices, in the order they make primitives
    std::array<std::optional<VertexInput>, vertex::k_input_count> inputs;
    vertex::Program vertex_program;
    fragment::Program fragment_program;
    Vec4 viewport_scale{}; // window = clip / w * scale + offset
    Vec4 viewport_offset{};
    Rect area; // pixels that may change: the surface clipped by the scissor
    ColorSurface target;
    u32 color_mask = 0; // method::k_color_mask
    bool flat_shading = false;
    std::optional<AlphaTest> alpha_test;
    std::optional<DepthTest> depth_test; // none: no depth test, and no depth writes
    std::optional<Blend> blend;
    Cull cull = Cull::none;
    // Whether counterclockwise triangles face the viewer, as they appear on the display.
    bool front_ccw = true;
    // The textures the fragment program samples, by unit.
    std::array<std::optional<Texture>, k_texture_units> textures;
};

// A frame as it reaches the display: 8-bit RGB, top row first.
struct Frame {
    u64 number = 0; // counting from 1
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgb;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void clear(const ClearRequest& request) = 0;
    // Returns an error if the draw needs something the backend doesn't support yet.
    virtual std::optional<std::string> draw(const DrawRequest& request) = 0;
    // Writes everything rendered so far back to guest memory.
    virtual void flush() {}
};

} // namespace rsx
