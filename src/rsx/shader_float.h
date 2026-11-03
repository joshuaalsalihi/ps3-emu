#pragma once

// Single-precision arithmetic for the RSX's shaders, computed exactly in software so frames are
// identical on every host. Values are IEEE 754 single bit patterns.
//
// UNVERIFIED: the RSX's exact floating-point behavior. We assume IEEE results rounded to nearest
// even, denormals kept, any NaN operand giving the default NaN, and MAD rounding twice (a
// multiply, then an add) like most GPUs of its generation. RCP and RSQ are exact here; the
// hardware's are approximations.

#include "common/types.h"

#include <array>

namespace rsx::shader_float {

using Float = u32;
using Vec4 = std::array<Float, 4>;

constexpr Float k_zero = 0;
constexpr Float k_one = 0x3f80'0000;
constexpr Float k_default_nan = 0x7fc0'0000;

Float add(Float a, Float b);
Float multiply(Float a, Float b);
Float multiply_add(Float a, Float b, Float c); // a * b + c
Float divide(Float a, Float b);
Float reciprocal(Float a);
Float reciprocal_square_root(Float a);
Float minimum(Float a, Float b);
Float maximum(Float a, Float b);
bool less(Float a, Float b); // false if either is NaN
bool equal(Float a, Float b);
Float floor(Float a);
// log2, to about 24 bits after the binary point; -infinity for zero, NaN below it.
Float log2(Float a);
// 2^a, from a's top 24 fraction bits.
Float exp2(Float a);

// The precisions a fragment program instruction can compute in, applied to a single-precision
// result. UNVERIFIED: fp16 as IEEE half precision (rounded to nearest, overflowing to infinity),
// and fx12 as 12-bit fixed point: multiples of 2^-10 in [-2, 2), saturating.
Float round_to_half(Float a);
Float round_to_fixed12(Float a);
Float from_integer(s64 value);
// Rounds to the nearest integer (ties to even), saturating to the s64 range. NaN gives 0.
s64 to_integer(Float a);

} // namespace rsx::shader_float
