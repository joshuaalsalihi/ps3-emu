#pragma once

// The SPU's single-precision floating point ("extended-range mode"), which is not IEEE 754.
// Reference: SPU ISA v1.2, section 9.1.
//
//  - Exponent 255 is an ordinary number: there are no infinities or NaNs, and the largest
//    magnitude, Smax, is 0x7fffffff (just under 2^129).
//  - Denormals are zero, as inputs and as results.
//  - Results are truncated (rounded toward zero). Too large saturates to Smax with the right
//    sign; too small becomes zero. A zero result is always +0.
//
// Values are passed as bit patterns. The arithmetic is soft_float's, so every host agrees.

#include "common/types.h"

namespace spu::spu_float {

constexpr u32 k_largest = 0x7fff'ffff; // Smax

// The exception conditions of section 9.1, for one word slot.
struct Flags {
    bool overflow = false;  // the exact result's magnitude exceeded Smax
    bool underflow = false; // the exact result was nonzero but below the smallest normal
    bool diff = false;      // the result may differ from IEEE's (see section 9.1)
    bool divide_by_zero = false;
};

// a + b
u32 add(u32 a, u32 b, Flags& flags);
// a × b
u32 multiply(u32 a, u32 b, Flags& flags);
// a × b + c, with an exact product and a single rounding.
u32 multiply_add(u32 a, u32 b, u32 c, Flags& flags);

// csflt and cuflt: an integer divided by 2^scale.
u32 from_integer(s64 value, s32 scale, Flags& flags);
// cflts and cfltu: x × 2^scale, truncated to an integer of `bits` bits (32 signed, or 64 for
// the unsigned conversion to clamp), saturating.
s64 to_integer(u32 x, s32 scale, u32 bits);

// Comparisons. Values with a zero exponent all equal zero, whatever their sign or fraction.
bool equal(u32 a, u32 b);
bool greater(u32 a, u32 b);

// frest, frsqest and fi. The estimates pack a base value and a step into one word for fi to
// interpolate between; see spu_float.cpp.
u32 reciprocal_estimate(u32 x, Flags& flags);
u32 reciprocal_square_root_estimate(u32 x, Flags& flags);
u32 interpolate(u32 fraction_source, u32 estimate, Flags& flags);

} // namespace spu::spu_float
