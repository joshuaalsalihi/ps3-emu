#pragma once

// Software IEEE 754 binary floating point, exact and independent of the host's FPU.
//
// Values are passed as the bit patterns of doubles. Results in single precision are rounded to
// single precision and then held in double format, as PowerPC floating-point registers hold them.
//
// Each operation computes the exact result, then rounds it once (round_to_format). Rounding
// follows PowerPC's rules, which are IEEE 754's plus two details:
//  - "tiny" results (for underflow) are detected before rounding;
//  - with overflow or underflow exceptions enabled, the exponent of the rounded result is
//    wrapped by 1536 (double) or 192 (single) instead of producing infinity or a denormal.
// Reference: Power ISA v3.1, Book I, section 4.3.6 "Rounding" and 4.4 "Floating-Point Exceptions".
//
// NaN operands are the caller's job: PowerPC and VMX select and quiet NaNs differently. Each
// operation documents the invalid operand combinations the caller must rule out first.

#include "common/types.h"

namespace soft_float {

enum class Rounding : u8 { nearest_even, toward_zero, toward_positive, toward_negative };

// A binary floating-point format: significand precision (including the implicit bit) and the
// exponent range of normal numbers.
struct Format {
    u32 precision;
    s32 min_exponent;
    s32 max_exponent;
    s32 exponent_wrap; // how far the exponent moves when overflow/underflow exceptions are enabled
};

inline constexpr Format k_double = {53, -1022, 1023, 1536};
inline constexpr Format k_single = {24, -126, 127, 192};

struct Environment {
    Rounding rounding = Rounding::nearest_even;
    bool overflow_enabled = false;  // PowerPC FPSCR[OE]
    bool underflow_enabled = false; // PowerPC FPSCR[UE]
};

// What happened while rounding. Flags are only ever set, never cleared.
struct Flags {
    bool overflow = false;
    bool underflow = false;
    bool inexact = false;
    bool divide_by_zero = false;
    bool fraction_rounded = false; // the significand was incremented (PowerPC FPSCR[FR])
};

// ---- Classifying double bit patterns ----

constexpr u64 k_sign_bit = u64{1} << 63;
constexpr u64 k_quiet_bit = u64{1} << 51;
constexpr u64 k_positive_infinity = 0x7ff0'0000'0000'0000;
constexpr u64 k_default_nan = 0x7ff8'0000'0000'0000;

constexpr bool is_negative(u64 bits) {
    return (bits & k_sign_bit) != 0;
}
constexpr bool is_nan(u64 bits) {
    return (bits & ~k_sign_bit) > k_positive_infinity;
}
constexpr bool is_signaling_nan(u64 bits) {
    return is_nan(bits) && (bits & k_quiet_bit) == 0;
}
constexpr bool is_infinity(u64 bits) {
    return (bits & ~k_sign_bit) == k_positive_infinity;
}
constexpr bool is_zero(u64 bits) {
    return (bits & ~k_sign_bit) == 0;
}
constexpr u64 negate(u64 bits) {
    return bits ^ k_sign_bit;
}

// True if the value is nonzero and below the smallest normal number of `format`.
bool is_denormal(u64 bits, Format format);

// ---- Arithmetic ----

// a + b. The caller rules out NaNs and infinities of opposite sign.
u64 add(u64 a, u64 b, Format format, Environment environment, Flags& flags);

// a * b. The caller rules out NaNs and infinity times zero.
u64 multiply(u64 a, u64 b, Format format, Environment environment, Flags& flags);

// a / b. The caller rules out NaNs, 0 / 0 and infinity / infinity. A finite nonzero value divided
// by zero gives infinity and sets divide_by_zero.
u64 divide(u64 a, u64 b, Format format, Environment environment, Flags& flags);

// The square root of a. The caller rules out NaNs and negative nonzero values (-0 gives -0).
u64 square_root(u64 a, Format format, Environment environment, Flags& flags);

// a * c + b with a single rounding. The caller rules out NaNs, infinity times zero, and an
// infinite product plus an infinity of the opposite sign.
u64 multiply_add(u64 a, u64 c, u64 b, Format format, Environment environment, Flags& flags);

// ---- Conversions ----

// Rounds a value to `format` (PowerPC frsp). The caller rules out NaNs.
u64 round_to_format(u64 a, Format format, Environment environment, Flags& flags);

// Converts a signed 64-bit integer to floating point.
u64 from_integer(s64 value, Format format, Environment environment, Flags& flags);

struct IntegerResult {
    s64 value;
    bool out_of_range; // the value was saturated
};

// Converts to a signed integer of `bits` bits (32 or 64), saturating if out of range. The
// caller rules out NaNs. Sets inexact and fraction_rounded, but never overflow.
IntegerResult to_integer(u64 a, u32 bits, Rounding rounding, Flags& flags);

// Rounds to an integral value, keeping the floating-point format (VMX vrfi*). No flags.
u64 round_to_integral(u64 a, Rounding rounding);

} // namespace soft_float
