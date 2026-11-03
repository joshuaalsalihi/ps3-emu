#include "gpu/texture.h"

#include "rsx/shader_ops.h"

#include <algorithm>

namespace gpu {

using namespace rsx::shader_float;
using rsx::Texture;
using rsx::TextureFilter;
using rsx::TextureFormat;
using rsx::TextureWrap;
using rsx::Vec4;
using rsx::fragment::Lookup;
using rsx::fragment::Quad;

namespace {

constexpr Float k_half = 0x3f00'0000;
constexpr Float k_1_256 = 0x3b80'0000; // 1/256, the unit of the LOD registers

u32 bytes_per_texel(TextureFormat format) {
    switch (format) {
    case TextureFormat::a8r8g8b8:
    case TextureFormat::d8r8g8b8: return 4;
    default: return 2;
    }
}

// `value` out of `maximum`, as a fraction in 0..1.
Float unorm(u32 value, u32 maximum) {
    return divide(from_integer(value), from_integer(maximum));
}

// Decodes one texel into alpha, red, green and blue: the order the remap names them in.
std::array<Float, 4> decode(TextureFormat format, u32 bits) {
    const auto field = [&](u32 shift, u32 width) {
        return unorm((bits >> shift) & ((1u << width) - 1), (1u << width) - 1);
    };
    switch (format) {
    case TextureFormat::a8r8g8b8: return {field(24, 8), field(16, 8), field(8, 8), field(0, 8)};
    case TextureFormat::d8r8g8b8: return {k_one, field(16, 8), field(8, 8), field(0, 8)};
    case TextureFormat::a1r5g5b5: return {field(15, 1), field(10, 5), field(5, 5), field(0, 5)};
    case TextureFormat::d1r5g5b5: return {k_one, field(10, 5), field(5, 5), field(0, 5)};
    case TextureFormat::a4r4g4b4: return {field(12, 4), field(8, 4), field(4, 4), field(0, 4)};
    case TextureFormat::r5g6b5: return {k_one, field(11, 5), field(5, 6), field(0, 5)};
    }
    return {};
}

// The remap (GCM_TEXTURE_REMAP_*): for each output channel (A, R, G, B), a 2-bit choice of
// source channel in bits 0-7 and a 2-bit type in bits 8-15: zero, one, or the chosen source.
Vec4 remap(u32 remap_bits, const std::array<Float, 4>& argb) {
    std::array<Float, 4> out{};
    for (u32 channel = 0; channel < 4; ++channel) {
        const u32 source = (remap_bits >> (2 * channel)) & 3;
        switch ((remap_bits >> (8 + 2 * channel)) & 3) {
        case 0: out[channel] = k_zero; break;
        case 1: out[channel] = k_one; break;
        default: out[channel] = argb[source]; break;
        }
    }
    return {out[1], out[2], out[3], out[0]};
}

// The byte offset of texel (x, y) in a swizzled level.
u32 swizzled_offset(u32 x, u32 y, LevelSize size) {
    u32 offset = 0;
    u32 bit = 0;
    for (u32 i = 0; (1u << i) < size.width || (1u << i) < size.height; ++i) {
        if ((1u << i) < size.width) offset |= ((x >> i) & 1) << bit++;
        if ((1u << i) < size.height) offset |= ((y >> i) & 1) << bit++;
    }
    return offset;
}

u32 level_address(const Texture& texture, u32 level) {
    const u32 bytes = bytes_per_texel(texture.format);
    u32 address = texture.address;
    for (u32 i = 0; i < level; ++i) {
        const LevelSize size = level_size(texture, i);
        address += texture.linear ? texture.pitch * size.height : size.width * size.height * bytes;
    }
    return address;
}

// Where a texel coordinate outside 0..size-1 lands; none means the border.
std::optional<s64> wrap(s64 coordinate, u32 size, TextureWrap mode) {
    const s64 n = size;
    switch (mode) {
    case TextureWrap::repeat: return ((coordinate % n) + n) % n;
    case TextureWrap::mirrored_repeat: {
        const s64 m = ((coordinate % (2 * n)) + 2 * n) % (2 * n);
        return m < n ? m : 2 * n - 1 - m;
    }
    case TextureWrap::clamp_to_edge: return std::clamp<s64>(coordinate, 0, n - 1);
    case TextureWrap::border:
        if (coordinate < 0 || coordinate >= n) return std::nullopt;
        return coordinate;
    }
    return coordinate;
}

Float subtract(Float a, Float b) {
    return add(a, b ^ rsx::shader_ops::k_sign);
}

s64 floor_to_integer(Float value) {
    return to_integer(floor(value));
}

s64 ceil_to_integer(Float value) {
    return -floor_to_integer(value ^ rsx::shader_ops::k_sign);
}

Float fraction(Float value) {
    return subtract(value, floor(value));
}

Float lerp(Float a, Float b, Float t) {
    return multiply_add(t, subtract(b, a), a);
}

Vec4 lerp(const Vec4& a, const Vec4& b, Float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)};
}

bool uses_mipmaps(TextureFilter filter) {
    return filter != TextureFilter::nearest && filter != TextureFilter::linear;
}

bool is_linear(TextureFilter filter) {
    return filter == TextureFilter::linear || filter == TextureFilter::linear_mipmap_nearest ||
           filter == TextureFilter::linear_mipmap_linear;
}

// log2 of how many texels one pixel step covers, from the quad's coordinates in texels:
// OpenGL's ρ, the larger of the two screen axes' rates of change.
Float lod_from_derivatives(const Quad<Float>& u, const Quad<Float>& v) {
    const auto rate_squared = [&](u32 neighbor) {
        const Float du = subtract(u[neighbor], u[0]);
        const Float dv = subtract(v[neighbor], v[0]);
        return multiply_add(dv, dv, multiply(du, du));
    };
    const Float rho_squared = maximum(rate_squared(1), rate_squared(2));
    return multiply(log2(rho_squared), k_half);
}

} // namespace

LevelSize level_size(const Texture& texture, u32 level) {
    return {std::max(texture.width >> level, 1u), std::max(texture.height >> level, 1u)};
}

Vec4 read_texel(const Memory& memory, const Texture& texture, u32 level, u32 x, u32 y) {
    const LevelSize size = level_size(texture, level);
    const u32 bytes = bytes_per_texel(texture.format);
    const u32 offset =
        texture.linear ? y * texture.pitch + x * bytes : swizzled_offset(x, y, size) * bytes;
    const u32 address = level_address(texture, level) + offset;
    const u32 bits = bytes == 4 ? memory.read<u32>(address) : memory.read<u16>(address);
    return remap(texture.remap, decode(texture.format, bits));
}

Quad<Vec4> TextureSampler::sample(u32 unit, const Quad<Vec4>& coordinates, Lookup lookup) const {
    const Texture& texture = *request_.textures[unit];

    // The coordinates in level 0's texels, for the level of detail.
    const Float width = texture.normalized ? from_integer(texture.width) : k_one;
    const Float height = texture.normalized ? from_integer(texture.height) : k_one;
    Quad<Float> u;
    Quad<Float> v;
    for (u32 lane = 0; lane < u.size(); ++lane) {
        u[lane] = multiply(coordinates[lane][0], width);
        v[lane] = multiply(coordinates[lane][1], height);
    }
    const Float quad_lod = lod_from_derivatives(u, v);

    const Float bias = multiply(from_integer(texture.lod_bias), k_1_256);
    const Float min_lod = multiply(from_integer(texture.min_lod), k_1_256);
    const Float max_lod = multiply(from_integer(texture.max_lod), k_1_256);
    Quad<Vec4> result;
    for (u32 lane = 0; lane < result.size(); ++lane) {
        Float lod = quad_lod;
        if (lookup == Lookup::level) lod = coordinates[lane][3];
        if (lookup == Lookup::bias) lod = add(lod, coordinates[lane][3]);
        lod = minimum(maximum(add(lod, bias), min_lod), max_lod);
        result[lane] = filter(texture, lod, coordinates[lane]);
    }
    return result;
}

// Chooses the filter and level(s) for a level of detail, as OpenGL does: magnification at 0 or
// below, minification above, between levels if the minification filter says so.
Vec4 TextureSampler::filter(const Texture& texture, Float lod, const Vec4& coordinates) const {
    if (!less(k_zero, lod)) {
        return sample_level(texture, 0, is_linear(texture.magnify), coordinates);
    }
    const bool linear = is_linear(texture.minify);
    if (!uses_mipmaps(texture.minify) || !texture.normalized) {
        return sample_level(texture, 0, linear, coordinates);
    }

    const s64 last = texture.levels - 1;
    if (texture.minify == TextureFilter::nearest_mipmap_nearest ||
        texture.minify == TextureFilter::linear_mipmap_nearest) {
        // The nearest level: ceil(lod + 1/2) - 1, which is 0 up to lod = 1/2.
        const s64 level = std::clamp<s64>(ceil_to_integer(add(lod, k_half)) - 1, 0, last);
        return sample_level(texture, static_cast<u32>(level), linear, coordinates);
    }
    const s64 lower = std::min(floor_to_integer(lod), last);
    if (lower == last) return sample_level(texture, static_cast<u32>(lower), linear, coordinates);
    return lerp(sample_level(texture, static_cast<u32>(lower), linear, coordinates),
                sample_level(texture, static_cast<u32>(lower + 1), linear, coordinates),
                fraction(lod));
}

Vec4 TextureSampler::sample_level(const Texture& texture, u32 level, bool linear,
                                  const Vec4& coordinates) const {
    const LevelSize size = level_size(texture, level);
    const Float u =
        texture.normalized ? multiply(coordinates[0], from_integer(size.width)) : coordinates[0];
    const Float v =
        texture.normalized ? multiply(coordinates[1], from_integer(size.height)) : coordinates[1];
    if (!linear) return texel_or_border(texture, level, floor_to_integer(u), floor_to_integer(v));

    // The four texels whose centers surround the sample point, weighted by its distance from
    // them.
    const Float x = subtract(u, k_half);
    const Float y = subtract(v, k_half);
    const s64 x0 = floor_to_integer(x);
    const s64 y0 = floor_to_integer(y);
    const Float a = fraction(x);
    const Float b = fraction(y);
    const Vec4 top = lerp(texel_or_border(texture, level, x0, y0),
                          texel_or_border(texture, level, x0 + 1, y0), a);
    const Vec4 bottom = lerp(texel_or_border(texture, level, x0, y0 + 1),
                             texel_or_border(texture, level, x0 + 1, y0 + 1), a);
    return lerp(top, bottom, b);
}

Vec4 TextureSampler::texel_or_border(const Texture& texture, u32 level, s64 x, s64 y) const {
    const LevelSize size = level_size(texture, level);
    const auto wrapped_x = wrap(x, size.width, texture.wrap_s);
    const auto wrapped_y = wrap(y, size.height, texture.wrap_t);
    if (!wrapped_x || !wrapped_y) {
        // UNVERIFIED: the border color isn't remapped.
        const u32 c = texture.border_color;
        return {unorm((c >> 16) & 0xff, 255), unorm((c >> 8) & 0xff, 255), unorm(c & 0xff, 255),
                unorm(c >> 24, 255)};
    }
    return read_texel(memory_, texture, level, static_cast<u32>(*wrapped_x),
                      static_cast<u32>(*wrapped_y));
}

} // namespace gpu
