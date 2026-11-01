#include "cpu/spu/spu_float.h"

#include "common/soft_float.h"
#include "common/u128.h"

#include <algorithm>
#include <bit>
#include <initializer_list>

namespace spu::spu_float {

namespace {

namespace sf = soft_float;

// Extended range runs one exponent step further than IEEE single precision, because exponent
// 255 is an ordinary number. Overflow then saturates to Smax, since results are truncated.
constexpr sf::Format k_extended = {24, -126, 128, 192};

constexpr u32 k_sign = 0x8000'0000;

u32 exponent_of(u32 x) {
    return (x >> 23) & 0xff;
}
u32 fraction_of(u32 x) {
    return x & 0x7f'ffff;
}

// Inputs IEEE would see differently: infinities and NaNs (exponent 255), and denormals.
bool differs_from_ieee(u32 x) {
    const u32 exponent = exponent_of(x);
    return exponent == 255 || (exponent == 0 && fraction_of(x) != 0);
}

// An operand as a double bit pattern for soft_float. A zero exponent means zero.
u64 decode(u32 x) {
    const u64 sign = u64{x >> 31} << 63;
    const u32 exponent = exponent_of(x);
    if (exponent == 0) return sign;
    return sign | (u64{exponent - 127 + 1023} << 52) | (u64{fraction_of(x)} << 29);
}

// Truncation, and "underflow enabled" so that soft_float flags every result that was tiny
// before rounding; encode() turns those into zero.
sf::Environment environment() {
    sf::Environment environment;
    environment.rounding = sf::Rounding::toward_zero;
    environment.underflow_enabled = true;
    return environment;
}

// A soft_float result as an operand, noting the exception conditions.
u32 encode(u64 bits, const sf::Flags& result_flags, Flags& flags) {
    if (result_flags.underflow) {
        flags.underflow = true;
        flags.diff = true;
        return 0;
    }
    if (sf::is_zero(bits)) return 0; // zero results are always +0

    const u32 sign = sf::is_negative(bits) ? k_sign : 0;
    const u32 exponent = static_cast<u32>((bits >> 52) & 0x7ff) - 1023 + 127;
    const u32 result = sign | (exponent << 23) | static_cast<u32>((bits >> 29) & 0x7f'ffff);

    // Overflow means the exact result exceeded Smax, even if truncation brought it back down.
    const bool saturated = (result & ~k_sign) == k_largest && result_flags.inexact;
    if (result_flags.overflow || saturated) flags.overflow = true;
    if (exponent == 255) flags.diff = true;
    return result;
}

void note_operands(Flags& flags, std::initializer_list<u32> operands) {
    for (const u32 x : operands) {
        if (differs_from_ieee(x)) flags.diff = true;
    }
}

// 2^n as a double bit pattern.
u64 power_of_two(s32 n) {
    return static_cast<u64>(n + 1023) << 52;
}

// ---- Estimates ----
// frest and frsqest return a base and a step for fi to interpolate with:
//
//   bits 0, 1-8, 9-21, 22-31:  sign, biased exponent, base fraction, step fraction
//   fi computes (-1)^sign × (1.base - 0.000step × Y) × 2^(exponent - 127)
//
// where Y is the low 19 fraction bits of x (as a fraction 0.Y), the position of x within an
// interval of inputs sharing its top four fraction bits. So each estimate describes a line
// across that interval. Section 9, "Floating Reciprocal Estimate".
//
// UNVERIFIED: the hardware's lines come from a table we don't have. Ours start below the curve
// by half the gap between the chord and the curve at the interval's midpoint, then meet the
// curve at the interval's end. Both curves are convex, so this halves the chord's error near
// the start, and the line never drops below the curve at the end (a value just above the
// smallest normal number stays representable). Everything is integer arithmetic.

constexpr u64 k_one = u64{1} << 32; // fixed point with 32 fraction bits

// Packs a line for the interval into an estimate. `start`, `middle` and `end` are the function
// at the start, midpoint and end of the interval, in fixed point and at most 1.0; the true
// value is that times 2^scale_exponent.
u32 pack_estimate(u32 sign, s32 scale_exponent, u64 start, u64 middle, u64 end) {
    const u64 chord_middle = (start + end) / 2;
    u64 base = start - (chord_middle - middle) / 2;

    // Normalize so the base is 1.base.
    s32 exponent = scale_exponent;
    while (base < k_one) {
        base <<= 1;
        end <<= 1;
        --exponent;
    }
    const s32 biased = exponent + 127;
    if (biased <= 0) return 0; // too small to represent

    // Keep 13 fraction bits of the base, then round the step down so the line still ends at or
    // above `end`. A step is 10 bits in units of 2^-13.
    const u64 kept_base = base & ~((u64{1} << 19) - 1);
    const u32 base_fraction = static_cast<u32>(kept_base >> 19) & 0x1fff;
    const u32 step_fraction = static_cast<u32>(std::min<u64>((kept_base - end) >> 19, 0x3ff));
    return sign | (static_cast<u32>(biased) << 23) | (base_fraction << 10) | step_fraction;
}

// The top four fraction bits pick the interval. The function is sampled at 1/32 steps:
// sample 2i is the start of interval i, 2i + 1 its middle and 2i + 2 its end.
u32 interval_of(u32 x) {
    return fraction_of(x) >> 19;
}

// 1/m in fixed point, for m = 1 + sample/32.
u64 reciprocal_sample(u32 sample) {
    return (k_one * 32) / (32 + sample);
}

u64 integer_square_root(u128 n) {
    u128 result = 0;
    u128 bit = u128{1} << 126;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= result + bit) {
            n -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<u64>(result);
}

// 1/sqrt(m) in fixed point, for m = (1 + sample/32) × factor.
u64 reciprocal_square_root_sample(u32 sample, u32 factor) {
    const u128 numerator = (u128{1} << 64) * 32;
    return integer_square_root(numerator / ((32 + sample) * factor));
}

} // namespace

u32 add(u32 a, u32 b, Flags& flags) {
    note_operands(flags, {a, b});
    sf::Flags result_flags;
    return encode(sf::add(decode(a), decode(b), k_extended, environment(), result_flags),
                  result_flags, flags);
}

u32 multiply(u32 a, u32 b, Flags& flags) {
    note_operands(flags, {a, b});
    sf::Flags result_flags;
    return encode(sf::multiply(decode(a), decode(b), k_extended, environment(), result_flags),
                  result_flags, flags);
}

u32 multiply_add(u32 a, u32 b, u32 c, Flags& flags) {
    note_operands(flags, {a, b, c});
    sf::Flags result_flags;
    const u64 result =
        sf::multiply_add(decode(a), decode(b), decode(c), k_extended, environment(), result_flags);
    return encode(result, result_flags, flags);
}

u32 from_integer(s64 value, s32 scale, Flags& flags) {
    // Converting a 32-bit integer to double is exact, so the scaled product is rounded once.
    sf::Flags exact;
    const u64 converted = sf::from_integer(value, sf::k_double, {}, exact);
    sf::Flags result_flags;
    const u64 scaled =
        sf::multiply(converted, power_of_two(-scale), k_extended, environment(), result_flags);
    return encode(scaled, result_flags, flags);
}

s64 to_integer(u32 x, s32 scale, u32 bits) {
    if (exponent_of(x) == 0) return 0;
    sf::Flags ignored;
    const u64 scaled = sf::multiply(decode(x), power_of_two(scale), sf::k_double, {}, ignored);
    return sf::to_integer(scaled, bits, sf::Rounding::toward_zero, ignored).value;
}

bool equal(u32 a, u32 b) {
    // Neither operand can be a NaN, so comparing as host doubles is exact.
    return std::bit_cast<double>(decode(a)) == std::bit_cast<double>(decode(b));
}

bool greater(u32 a, u32 b) {
    return std::bit_cast<double>(decode(a)) > std::bit_cast<double>(decode(b));
}

u32 reciprocal_estimate(u32 x, Flags& flags) {
    const u32 sign = x & k_sign;
    const u32 exponent = exponent_of(x);
    if (exponent == 0) {
        flags.divide_by_zero = true;
        return sign | k_largest;
    }
    // 1/x = (1/m) × 2^(127 - exponent), where m is x's significand.
    const u32 i = interval_of(x);
    return pack_estimate(sign, 127 - static_cast<s32>(exponent), reciprocal_sample(2 * i),
                         reciprocal_sample(2 * i + 1), reciprocal_sample(2 * i + 2));
}

u32 reciprocal_square_root_estimate(u32 x, Flags& flags) {
    const u32 exponent = exponent_of(x);
    if (exponent == 0) {
        flags.divide_by_zero = true;
        return k_largest;
    }
    // For |x| = m × 2^e with e even, 1/sqrt(|x|) = (1/sqrt(m)) × 2^(-e/2). With e odd, use
    // 2m and e - 1 instead. The result is always positive.
    const s32 e = static_cast<s32>(exponent) - 127;
    const bool odd = (e & 1) != 0;
    const u32 factor = odd ? 2 : 1;
    const s32 half_exponent = -(odd ? e - 1 : e) / 2;
    const u32 i = interval_of(x);
    return pack_estimate(0, half_exponent, reciprocal_square_root_sample(2 * i, factor),
                         reciprocal_square_root_sample(2 * i + 1, factor),
                         reciprocal_square_root_sample(2 * i + 2, factor));
}

u32 interpolate(u32 fraction_source, u32 estimate, Flags& flags) {
    note_operands(flags, {estimate});
    const u32 exponent = exponent_of(estimate);
    if (exponent == 0) return 0;

    // (1.base - 0.000step × Y) in fixed point with 32 fraction bits.
    const u64 base = u64{0x2000 | ((estimate >> 10) & 0x1fff)} << 19;
    const u64 step = estimate & 0x3ff;
    const u64 y = fraction_source & 0x7'ffff;
    u64 value = base - step * y;

    // The result is just below 2.0 at most, and at least 1.0 - 2^-3; normalize and truncate.
    s32 result_exponent = static_cast<s32>(exponent);
    while (value < k_one) {
        value <<= 1;
        --result_exponent;
    }
    if (result_exponent <= 0) {
        flags.underflow = true;
        flags.diff = true;
        return 0;
    }
    if (result_exponent == 255) flags.diff = true;
    return (estimate & k_sign) | (static_cast<u32>(result_exponent) << 23) |
           (static_cast<u32>(value >> 9) & 0x7f'ffff);
}

} // namespace spu::spu_float
