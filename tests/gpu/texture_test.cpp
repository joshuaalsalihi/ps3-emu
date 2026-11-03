// Texture decoding and the reference sampler. Expected values follow OpenGL's sampling rules
// (texel centers at half-integers, the level of detail from derivatives), which we assume the
// RSX shares; the layouts follow the comments in gpu/texture.h.

#include "gpu/texture.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>

using namespace rsx;
using rsx::fragment::Lookup;
using rsx::fragment::Quad;

namespace {

constexpr u32 k_base = 0x10000;
constexpr u32 k_identity_remap = 0xaae4; // each channel reads itself

shader_float::Float f(float value) {
    return std::bit_cast<u32>(value);
}

float to_float(shader_float::Float value) {
    return std::bit_cast<float>(value);
}

class TextureTest {
public:
    TextureTest() {
        memory.map(k_base, 0x10000, Memory::k_read_write);
        Texture texture;
        texture.address = k_base;
        texture.remap = k_identity_remap;
        request.textures[0] = texture;
    }

    // A linear A8R8G8B8 texture of the given size, filled by `texel(x, y)`.
    template <typename Texel>
    void make_linear(u32 width, u32 height, Texel texel) {
        Texture& t = *request.textures[0];
        t.format = TextureFormat::a8r8g8b8;
        t.linear = true;
        t.width = width;
        t.height = height;
        t.pitch = width * 4;
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                memory.write<u32>(k_base + y * t.pitch + x * 4, texel(x, y));
            }
        }
    }

    Texture& unit0() { return *request.textures[0]; }

    // Samples unit 0 at one point (all four lanes alike, so the level of detail is
    // -infinity: magnification) and returns the red channel.
    float red_at(float s, float t) {
        const Vec4 point = {f(s), f(t), f(0), f(1)};
        const gpu::TextureSampler sampler(memory, request);
        return to_float(sampler.sample(0, {point, point, point, point}, Lookup::plain)[0][0]);
    }

    Memory memory;
    DrawRequest request;
};

} // namespace

TEST_CASE_METHOD(TextureTest, "texel formats decode to fractions of their maximum") {
    unit0().linear = true;
    unit0().width = 4;
    unit0().pitch = 16;
    const auto texel = [&](TextureFormat format, u32 bits) {
        unit0().format = format;
        if (format == TextureFormat::a8r8g8b8 || format == TextureFormat::d8r8g8b8) {
            memory.write<u32>(k_base, bits);
        } else if (format == TextureFormat::b8) {
            memory.write<u8>(k_base, static_cast<u8>(bits));
        } else {
            memory.write<u16>(k_base, static_cast<u16>(bits));
        }
        const Vec4 value = gpu::read_texel(memory, unit0(), 0, 0, 0);
        return std::array<float, 4>{to_float(value[0]), to_float(value[1]), to_float(value[2]),
                                    to_float(value[3])};
    };
    using A = std::array<float, 4>;
    CHECK(texel(TextureFormat::a8r8g8b8, 0x80ff'0033) == A{1, 0, 0.2f, 128 / 255.0f});
    CHECK(texel(TextureFormat::d8r8g8b8, 0x00ff'0033) == A{1, 0, 0.2f, 1});
    CHECK(texel(TextureFormat::r5g6b5, 0xf801) == A{1, 0, 1 / 31.0f, 1});
    CHECK(texel(TextureFormat::r5g6b5, 0x07e0) == A{0, 1, 0, 1});
    CHECK(texel(TextureFormat::a1r5g5b5, 0x8000 | 31 << 5) == A{0, 1, 0, 1});
    CHECK(texel(TextureFormat::d1r5g5b5, 31 << 10) == A{1, 0, 0, 1});
    CHECK(texel(TextureFormat::a4r4g4b4, 0x5f03) == A{1, 0, 0.2f, 1 / 3.0f});
    CHECK(texel(TextureFormat::b8, 0x33) == A{0, 0, 0.2f, 0});
    CHECK(texel(TextureFormat::g8b8, 0xff33) == A{0, 1, 0.2f, 0});
}

TEST_CASE_METHOD(TextureTest, "the remap chooses each channel's source, or 0 or 1") {
    make_linear(1, 1, [](u32, u32) { return 0x4011'2233; });
    // Red reads blue and blue reads red (selectors: A = 0, R = 1, G = 2, B = 3); green is 1 and
    // alpha 0.
    unit0().remap = (2u << 14 | 1u << 12 | 2u << 10 | 0u << 8) | (1u << 6 | 3u << 2);
    const Vec4 value = gpu::read_texel(memory, unit0(), 0, 0, 0);
    CHECK(value == Vec4{f(0x33 / 255.0f), f(1), f(0x11 / 255.0f), f(0)});
}

TEST_CASE("swizzled textures interleave x and y bits, x first") {
    Memory memory;
    memory.map(k_base, 0x1000, Memory::k_read_write);
    Texture texture;
    texture.address = k_base;
    texture.remap = k_identity_remap;
    texture.format = TextureFormat::a8r8g8b8;
    const auto offset_of = [&](u32 width, u32 height, u32 x, u32 y) {
        texture.width = width;
        texture.height = height;
        // Mark every texel with its index, then see which index (x, y) reads.
        for (u32 i = 0; i < width * height; ++i) {
            memory.write<u32>(k_base + 4 * i, i);
        }
        return static_cast<u32>(
            std::bit_cast<float>(gpu::read_texel(memory, texture, 0, x, y)[2]) * 255.0f + 0.5f);
    };
    CHECK(offset_of(4, 4, 1, 0) == 1);
    CHECK(offset_of(4, 4, 0, 1) == 2);
    CHECK(offset_of(4, 4, 2, 1) == 6);
    CHECK(offset_of(4, 4, 3, 3) == 15);
    // 8x2: once y's single bit is used, x's remaining bits follow.
    CHECK(offset_of(8, 2, 4, 1) == 0b1010);
    CHECK(offset_of(8, 2, 7, 0) == 0b1101);
}

TEST_CASE_METHOD(TextureTest, "mipmap levels follow each other") {
    Texture& t = unit0();
    t.format = TextureFormat::r5g6b5;
    t.width = 4;
    t.height = 2;
    t.levels = 3;
    // Swizzled: level 0 is 4x2 (16 bytes), level 1 2x1 (4 bytes), level 2 1x1.
    memory.write<u16>(k_base + 16, 0xf800); // level 1, texel (0, 0): red
    memory.write<u16>(k_base + 20, 0x001f); // level 2: blue
    CHECK(gpu::read_texel(memory, t, 1, 0, 0)[0] == f(1));
    CHECK(gpu::read_texel(memory, t, 2, 0, 0)[2] == f(1));
    CHECK(gpu::level_size(t, 2).width == 1);
    CHECK(gpu::level_size(t, 2).height == 1);

    // Linear: each level keeps the pitch, so level 1 starts 2 rows of 64 bytes in.
    t.linear = true;
    t.pitch = 64;
    memory.write<u16>(k_base + 128, 0x07e0); // level 1: green
    CHECK(gpu::read_texel(memory, t, 1, 0, 0)[1] == f(1));
}

TEST_CASE_METHOD(TextureTest, "nearest filtering takes the texel the point is in") {
    make_linear(4, 1, [](u32 x, u32) { return x * 0x40 << 16; }); // red 0, 64, 128, 192
    CHECK(red_at(0.0f, 0.5f) == 0);
    CHECK(red_at(0.26f, 0.5f) == 64 / 255.0f);
    CHECK(red_at(0.99f, 0.5f) == 192 / 255.0f);
}

TEST_CASE_METHOD(TextureTest,
                 "linear filtering weights the texels whose centers surround the point") {
    make_linear(4, 1, [](u32 x, u32) { return x * 0x40 << 16; });
    unit0().magnify = TextureFilter::linear;
    unit0().wrap_s = TextureWrap::clamp_to_edge;
    // Texel centers are at 1/8, 3/8, ...: at 1/4, halfway between texels 0 and 1.
    CHECK(red_at(0.25f, 0.5f) == 32 / 255.0f);
    CHECK(red_at(0.375f, 0.5f) == 64 / 255.0f);
    CHECK(red_at(0.0f, 0.5f) == 0); // clamped: texel 0 on both sides
}

TEST_CASE_METHOD(TextureTest, "wrap modes place coordinates outside 0..1") {
    make_linear(4, 1, [](u32 x, u32) { return x * 0x40 << 16; });
    CHECK(red_at(1.3f, 0.5f) == 64 / 255.0f); // repeat: 0.3
    unit0().wrap_s = TextureWrap::mirrored_repeat;
    CHECK(red_at(1.3f, 0.5f) == 128 / 255.0f); // mirrored: 0.7
    CHECK(red_at(-0.2f, 0.5f) == 0);           // mirrored: 0.2
    unit0().wrap_s = TextureWrap::clamp_to_edge;
    CHECK(red_at(1.3f, 0.5f) == 192 / 255.0f);
    unit0().wrap_s = TextureWrap::border;
    unit0().border_color = 0xff11'0000;
    CHECK(red_at(1.3f, 0.5f) == 0x11 / 255.0f);
}

TEST_CASE_METHOD(TextureTest, "unnormalized coordinates count texels") {
    make_linear(4, 1, [](u32 x, u32) { return x * 0x40 << 16; });
    unit0().normalized = false;
    CHECK(red_at(2.5f, 0.5f) == 128 / 255.0f);
}

TEST_CASE_METHOD(TextureTest, "the level of detail comes from the quad's derivatives") {
    // A 4x4 texture whose levels are solid red (0), green (1) and blue (2), with nearest
    // mipmapping. Coordinates that step `step` texels per pixel have a LOD of log2(step).
    Texture& t = unit0();
    t.format = TextureFormat::a8r8g8b8;
    t.width = 4;
    t.height = 4;
    t.levels = 3;
    t.max_lod = 12 << 8;
    t.minify = TextureFilter::nearest_mipmap_nearest;
    for (u32 i = 0; i < 16; ++i) {
        memory.write<u32>(k_base + 4 * i, 0xffff'0000);
    }
    for (u32 i = 0; i < 4; ++i) {
        memory.write<u32>(k_base + 64 + 4 * i, 0xff00'ff00);
    }
    memory.write<u32>(k_base + 80, 0xff00'00ff);

    const auto color_at_step = [&](float step) {
        const float d = step / 4;
        const Quad<Vec4> quad = {Vec4{f(0.1f), f(0.1f), 0, 0}, Vec4{f(0.1f + d), f(0.1f), 0, 0},
                                 Vec4{f(0.1f), f(0.1f + d), 0, 0},
                                 Vec4{f(0.1f + d), f(0.1f + d), 0, 0}};
        const gpu::TextureSampler sampler(memory, request);
        const Vec4 color = sampler.sample(0, quad, Lookup::plain)[0];
        return (color[0] == f(1) ? 0x100 : 0) | (color[1] == f(1) ? 0x010 : 0) |
               (color[2] == f(1) ? 0x001 : 0);
    };
    CHECK(color_at_step(1) == 0x100);    // LOD 0: magnification, level 0
    CHECK(color_at_step(1.2f) == 0x100); // LOD 0.26: the nearest level is still 0
    CHECK(color_at_step(1.6f) == 0x010); // LOD 0.68: level 1
    CHECK(color_at_step(2) == 0x010);    // LOD 1
    CHECK(color_at_step(4) == 0x001);    // LOD 2
    CHECK(color_at_step(64) == 0x001);   // clamped to the last level

    // TXL takes the level from w; the maximum LOD clamps it.
    const Vec4 point = {f(0.1f), f(0.1f), 0, f(1)};
    const gpu::TextureSampler sampler(memory, request);
    CHECK(sampler.sample(0, {point, point, point, point}, Lookup::level)[0][1] == f(1));
    t.max_lod = 0;
    const gpu::TextureSampler clamped(memory, request);
    CHECK(clamped.sample(0, {point, point, point, point}, Lookup::level)[0][0] == f(1));
}
