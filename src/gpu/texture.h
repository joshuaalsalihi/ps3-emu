#pragma once

// Textures as the RSX stores them, and the reference backend's sampling.
//
// Layout: a linear texture's rows are `pitch` bytes apart. A swizzled texture (power-of-two sizes
// only) stores its texels in Morton order: the bits of x and y interleave, x's lowest bit first,
// until the smaller dimension runs out of bits. Each mipmap level follows the one before it.
// UNVERIFIED: the RSX's swizzle (this is NVIDIA's usual one), and the place of a linear
// texture's smaller levels (we keep the pitch, as for level 0).
//
// Sampling follows OpenGL's rules (texel centers at half-integers, the level of detail from the
// quad's derivatives), computed with soft_float so every host gets the same bits.
// UNVERIFIED: the RSX filters with its own fixed-point precision; ours is exact arithmetic.

#include "core/memory.h"
#include "rsx/backend.h"

namespace gpu {

struct LevelSize {
    u32 width = 1;
    u32 height = 1;
};

LevelSize level_size(const rsx::Texture& texture, u32 level);

// Texel (x, y) of a mipmap level: red, green, blue and alpha in 0..1, after the texture's remap.
rsx::Vec4 read_texel(const Memory& memory, const rsx::Texture& texture, u32 level, u32 x, u32 y);

// Samples the textures of one draw request; Rsx has checked that each one is supported.
class TextureSampler : public rsx::fragment::Sampler {
public:
    TextureSampler(const Memory& memory, const rsx::DrawRequest& request)
        : memory_(memory), request_(request) {}

    rsx::fragment::Quad<rsx::Vec4> sample(u32 unit,
                                          const rsx::fragment::Quad<rsx::Vec4>& coordinates,
                                          rsx::fragment::Lookup lookup) const override;

private:
    rsx::Vec4 filter(const rsx::Texture& texture, rsx::shader_float::Float lod,
                     const rsx::Vec4& coordinates) const;
    rsx::Vec4 sample_level(const rsx::Texture& texture, u32 level, bool linear,
                           const rsx::Vec4& coordinates) const;
    rsx::Vec4 texel_or_border(const rsx::Texture& texture, u32 level, s64 x, s64 y) const;

    const Memory& memory_;
    const rsx::DrawRequest& request_;
};

} // namespace gpu
