#pragma once

// The bit conversions floating-point loads and stores do between single-precision memory values
// and double-precision registers. They involve no rounding.
// Reference: PowerPC Architecture Book I (v2.02), section 4.6, "Floating-Point Load
// Instructions" and "Floating-Point Store Instructions".

#include "common/types.h"

// lfs: converts a single-precision bit pattern to double precision. Exact for every input.
constexpr u64 single_to_double_bits(u32 word) {
    const u64 sign = u64{word >> 31} << 63;
    const u32 exponent = (word >> 23) & 0xff;
    const u64 fraction = word & 0x7f'ffff;

    // Denormalized: normalize the fraction, lowering the exponent as it shifts.
    if (exponent == 0 && fraction != 0) {
        s32 unbiased = -126;
        u64 bits = fraction << 29; // 52-bit double fraction, no implicit 1 yet
        while ((bits & (u64{1} << 52)) == 0) {
            bits <<= 1;
            --unbiased;
        }
        return sign | (static_cast<u64>(unbiased + 1023) << 52) | (bits & ((u64{1} << 52) - 1));
    }

    // Normalized, zero, infinity or NaN: widen the exponent by repeating its top bit (inverted
    // for normalized values, as the bias changes from 127 to 1023).
    const u64 top_bit = (word >> 30) & 1;
    const u64 fill = (exponent == 0 || exponent == 0xff) ? top_bit : (top_bit ^ 1);
    const u64 rest = word & 0x3fff'ffff; // the low 7 exponent bits and the fraction
    return sign | (top_bit << 62) | (fill * (u64{0b111} << 59)) | (rest << 29);
}

// stfs: converts a double-precision bit pattern to single precision. Values too small to be a
// single-precision normal number are denormalized, truncating the fraction.
constexpr u32 double_to_single_bits(u64 bits) {
    const u32 sign = static_cast<u32>(bits >> 63) << 31;
    const u32 exponent = (bits >> 52) & 0x7ff;

    // Exponent above single precision's smallest normal (or zero): keep the top bits.
    if (exponent > 896 || (bits & 0x7fff'ffff'ffff'ffff) == 0) {
        return static_cast<u32>(((bits >> 32) & 0xc000'0000) | ((bits >> 29) & 0x3fff'ffff));
    }

    // Too small for a normal single: shift the fraction (with its implicit 1) right until the
    // exponent reaches -126. UNVERIFIED: below exponent 874 the manual leaves the result
    // undefined; this produces a signed zero.
    s32 unbiased = static_cast<s32>(exponent) - 1023;
    u64 fraction = (u64{1} << 52) | (bits & ((u64{1} << 52) - 1));
    while (unbiased < -126 && fraction != 0) {
        fraction >>= 1;
        ++unbiased;
    }
    return sign | static_cast<u32>((fraction >> 29) & 0x7f'ffff);
}
