#include "common/soft_float.h"

#include "common/u128.h"

#include <bit>
#include <utility>

namespace soft_float {

namespace {

constexpr u64 k_fraction_mask = (u64{1} << 52) - 1;

// A finite, nonzero value: significand × 2^exponent.
struct Unpacked {
    bool negative;
    s32 exponent;
    u64 significand;
};

Unpacked unpack(u64 bits) {
    const u32 biased = (bits >> 52) & 0x7ff;
    const u64 fraction = bits & k_fraction_mask;
    if (biased == 0) return {is_negative(bits), -1074, fraction}; // denormal: no implicit bit
    return {is_negative(bits), static_cast<s32>(biased) - 1075, fraction | (u64{1} << 52)};
}

// Shifts a denormal's significand up so its top bit is bit 52, like a normal number's.
Unpacked normalized(Unpacked value) {
    const int shift = std::countl_zero(value.significand) - 11;
    return {value.negative, value.exponent - shift, value.significand << shift};
}

u32 highest_bit(u128 value) {
    const u64 high = static_cast<u64>(value >> 64);
    if (high != 0) return 127 - std::countl_zero(high);
    return 63 - std::countl_zero(static_cast<u64>(value));
}

// Encodes significand × 2^exponent as a double. The value must be exactly representable.
u64 pack(bool negative, s32 exponent, u64 significand) {
    const u64 sign = negative ? k_sign_bit : 0;
    if (significand == 0) return sign;

    // Move the top bit to bit 52, where a double's implicit bit goes.
    const s32 top = static_cast<s32>(highest_bit(significand));
    if (top > 52) {
        significand >>= top - 52; // only zero bits are dropped, as the value is representable
    } else {
        significand <<= 52 - top;
    }
    exponent += top - 52;

    const s32 biased = exponent + 1075;
    if (biased >= 1) return sign | (u64(biased) << 52) | (significand & k_fraction_mask);
    return sign | (significand >> (1 - biased)); // denormal
}

constexpr u64 infinity(bool negative) {
    return negative ? (k_sign_bit | k_positive_infinity) : k_positive_infinity;
}

// The sum of two zeros, or an exact cancellation like x - x: IEEE 754 makes it +0, except -0 when
// both are negative or when rounding toward minus infinity.
u64 zero_sum(bool a_negative, bool b_negative, Rounding rounding) {
    if (a_negative == b_negative) return a_negative ? k_sign_bit : 0;
    return rounding == Rounding::toward_negative ? k_sign_bit : 0;
}

// ---- Rounding ----

// How the bits dropped by rounding compare with half a unit in the last kept place.
enum class Remainder { zero, below_half, half, above_half };

struct Split {
    u128 kept;
    Remainder remainder;
};

// Drops the low `shift` bits of `value`. A negative shift moves it left instead (exactly).
Split split_off(u128 value, s64 shift) {
    if (shift <= 0) return {value << -shift, Remainder::zero};
    if (shift > 128) return {0, value == 0 ? Remainder::zero : Remainder::below_half};

    const u128 kept = shift == 128 ? 0 : value >> shift;
    const u128 dropped = shift == 128 ? value : value & ((u128{1} << shift) - 1);
    const u128 half = u128{1} << (shift - 1);
    if (dropped == 0) return {kept, Remainder::zero};
    if (dropped < half) return {kept, Remainder::below_half};
    if (dropped == half) return {kept, Remainder::half};
    return {kept, Remainder::above_half};
}

bool rounds_up(Rounding rounding, bool negative, const Split& split) {
    switch (rounding) {
    case Rounding::nearest_even:
        return split.remainder == Remainder::above_half ||
               (split.remainder == Remainder::half && (split.kept & 1) != 0);
    case Rounding::toward_zero: return false;
    case Rounding::toward_positive: return !negative && split.remainder != Remainder::zero;
    case Rounding::toward_negative: return negative && split.remainder != Remainder::zero;
    }
    return false;
}

// The result of an overflow with overflow exceptions disabled: infinity or the largest finite
// number, depending on the rounding direction.
u64 overflow_result(bool negative, Format format, Rounding rounding) {
    const bool to_infinity = rounding == Rounding::nearest_even ||
                             (rounding == Rounding::toward_positive && !negative) ||
                             (rounding == Rounding::toward_negative && negative);
    if (to_infinity) return infinity(negative);
    const u64 largest_significand = (u64{1} << format.precision) - 1;
    return pack(negative, format.max_exponent - static_cast<s32>(format.precision) + 1,
                largest_significand);
}

// Rounds the exact value ±significand × 2^exponent to `format`. This is where every operation's
// precision, exponent range and flags are decided.
u64 round(bool negative, s32 exponent, u128 significand, Format format, Environment environment,
          Flags& flags) {
    if (significand == 0) return negative ? k_sign_bit : 0;

    const s32 precision = static_cast<s32>(format.precision);
    const s32 top = exponent + static_cast<s32>(highest_bit(significand)); // value < 2^(top+1)
    const bool tiny = top < format.min_exponent; // detected before rounding

    // The weight of the result's least significant bit. A tiny result is denormalized: its bits
    // stop at the smallest denormal. With underflow exceptions enabled, it isn't.
    const bool denormalize = tiny && !environment.underflow_enabled;
    s32 lsb = (denormalize ? format.min_exponent : top) - (precision - 1);

    const Split split = split_off(significand, s64{lsb} - exponent);
    const bool increment = rounds_up(environment.rounding, negative, split);
    const bool inexact = split.remainder != Remainder::zero;
    u128 kept = split.kept + (increment ? 1 : 0);
    if (kept == (u128{1} << precision)) { // rounding carried into a new top bit
        kept >>= 1;
        ++lsb;
    }
    flags.inexact |= inexact;
    flags.fraction_rounded |= increment;

    if (kept == 0) { // a tiny value rounded to zero
        flags.underflow |= inexact;
        return negative ? k_sign_bit : 0;
    }

    const s32 result_top = lsb + static_cast<s32>(highest_bit(kept));
    if (result_top > format.max_exponent) {
        flags.overflow = true;
        if (environment.overflow_enabled) {
            return pack(negative, lsb - format.exponent_wrap, static_cast<u64>(kept));
        }
        flags.inexact = true;
        return overflow_result(negative, format, environment.rounding);
    }

    if (tiny && environment.underflow_enabled) {
        flags.underflow = true;
        return pack(negative, lsb + format.exponent_wrap, static_cast<u64>(kept));
    }
    if (tiny) flags.underflow |= inexact; // "tiny" and "loss of accuracy"
    return pack(negative, lsb, static_cast<u64>(kept));
}

// ---- Exact addition of two terms ----

// A finite, nonzero term in a 128-bit frame: its top bit is bit 125, leaving room for a carry
// and for at least 72 bits below a double's last bit.
struct Term {
    bool negative;
    s32 exponent;
    u128 significand;
};

constexpr u32 k_frame_top = 125;

Term to_frame(bool negative, s32 exponent, u128 significand) {
    const s32 shift = static_cast<s32>(k_frame_top - highest_bit(significand));
    return {negative, exponent - shift, significand << shift};
}

// Shifts right, folding any lost one bits into the lowest bit ("sticky"), so rounding still
// knows the value was not exact.
u128 shift_right_sticky(u128 value, s64 distance) {
    if (distance <= 0) return value;
    if (distance >= 128) return value != 0 ? 1 : 0;
    const bool lost = (value & ((u128{1} << distance) - 1)) != 0;
    return (value >> distance) | (lost ? 1 : 0);
}

// x + y, rounded once. Because both terms start exact with their top bits at bit 125, the
// smaller one only loses bits (into the sticky bit) when it is so much smaller that the result
// keeps far more precision than rounding needs.
u64 add_terms(Term x, Term y, Format format, Environment environment, Flags& flags) {
    const bool x_smaller =
        x.exponent < y.exponent || (x.exponent == y.exponent && x.significand < y.significand);
    if (x_smaller) std::swap(x, y);

    const u128 aligned = shift_right_sticky(y.significand, s64{x.exponent} - y.exponent);
    if (x.negative == y.negative) {
        return round(x.negative, x.exponent, x.significand + aligned, format, environment, flags);
    }
    const u128 difference = x.significand - aligned;
    if (difference == 0) return zero_sum(x.negative, y.negative, environment.rounding);
    return round(x.negative, x.exponent, difference, format, environment, flags);
}

Term term(u64 bits) {
    const Unpacked value = unpack(bits);
    return to_frame(value.negative, value.exponent, value.significand);
}

// Floor of the square root of `value`, and whether it was exact. The digit-by-digit method.
std::pair<u128, bool> integer_square_root(u128 value) {
    u128 root = 0;
    u128 remainder = value;
    u128 bit = u128{1} << 126; // the highest power of four that fits
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (remainder >= root + bit) {
            remainder -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return {root, remainder == 0};
}

} // namespace

bool is_denormal(u64 bits, Format format) {
    if (is_zero(bits) || is_infinity(bits) || is_nan(bits)) return false;
    const Unpacked value = unpack(bits);
    const s32 top = value.exponent + static_cast<s32>(highest_bit(value.significand));
    return top < format.min_exponent;
}

// ---- Arithmetic ----

u64 add(u64 a, u64 b, Format format, Environment environment, Flags& flags) {
    if (is_infinity(a)) return a;
    if (is_infinity(b)) return b;
    if (is_zero(a) && is_zero(b)) {
        return zero_sum(is_negative(a), is_negative(b), environment.rounding);
    }
    if (is_zero(a)) return round_to_format(b, format, environment, flags);
    if (is_zero(b)) return round_to_format(a, format, environment, flags);
    return add_terms(term(a), term(b), format, environment, flags);
}

u64 multiply(u64 a, u64 b, Format format, Environment environment, Flags& flags) {
    const bool negative = is_negative(a) != is_negative(b);
    if (is_infinity(a) || is_infinity(b)) return infinity(negative);
    if (is_zero(a) || is_zero(b)) return negative ? k_sign_bit : 0;

    const Unpacked x = unpack(a);
    const Unpacked y = unpack(b);
    return round(negative, x.exponent + y.exponent, u128{x.significand} * y.significand, format,
                 environment, flags);
}

u64 divide(u64 a, u64 b, Format format, Environment environment, Flags& flags) {
    const bool negative = is_negative(a) != is_negative(b);
    if (is_infinity(a)) return infinity(negative);
    if (is_infinity(b) || is_zero(a)) return negative ? k_sign_bit : 0;
    if (is_zero(b)) {
        flags.divide_by_zero = true;
        return infinity(negative);
    }

    // With both significands at 53 bits, shifting the dividend up by 74 gives a quotient of at
    // least 74 bits: plenty for rounding, with the remainder folded into a sticky bit.
    const Unpacked x = normalized(unpack(a));
    const Unpacked y = normalized(unpack(b));
    const u128 dividend = u128{x.significand} << 74;
    const u128 quotient = dividend / y.significand;
    const bool exact = dividend % y.significand == 0;
    return round(negative, x.exponent - 74 - y.exponent, quotient | (exact ? 0 : 1), format,
                 environment, flags);
}

u64 square_root(u64 a, Format format, Environment environment, Flags& flags) {
    if (is_zero(a) || is_infinity(a)) return a;

    const Unpacked value = normalized(unpack(a));
    u128 significand = value.significand;
    s32 exponent = value.exponent;

    // An even exponent halves exactly; the extra 72 bits give the root at least 62 bits.
    if (exponent % 2 != 0) {
        significand <<= 1;
        --exponent;
    }
    const auto [root, exact] = integer_square_root(significand << 72);
    return round(false, (exponent - 72) / 2, root | (exact ? 0 : 1), format, environment, flags);
}

u64 multiply_add(u64 a, u64 c, u64 b, Format format, Environment environment, Flags& flags) {
    const bool product_negative = is_negative(a) != is_negative(c);
    if (is_infinity(a) || is_infinity(c)) return infinity(product_negative);
    if (is_infinity(b)) return b;
    if (is_zero(a) || is_zero(c)) {
        if (is_zero(b)) return zero_sum(product_negative, is_negative(b), environment.rounding);
        return round_to_format(b, format, environment, flags);
    }

    const Unpacked x = unpack(a);
    const Unpacked y = unpack(c);
    const u128 product = u128{x.significand} * y.significand; // exact: at most 106 bits
    const s32 product_exponent = x.exponent + y.exponent;
    if (is_zero(b)) {
        return round(product_negative, product_exponent, product, format, environment, flags);
    }
    return add_terms(to_frame(product_negative, product_exponent, product), term(b), format,
                     environment, flags);
}

// ---- Conversions ----

u64 round_to_format(u64 a, Format format, Environment environment, Flags& flags) {
    if (is_zero(a) || is_infinity(a)) return a;
    const Unpacked value = unpack(a);
    return round(value.negative, value.exponent, value.significand, format, environment, flags);
}

u64 from_integer(s64 value, Format format, Environment environment, Flags& flags) {
    if (value == 0) return 0;
    const bool negative = value < 0;
    const u64 magnitude = negative ? 0 - static_cast<u64>(value) : static_cast<u64>(value);
    return round(negative, 0, magnitude, format, environment, flags);
}

IntegerResult to_integer(u64 a, u32 bits, Rounding rounding, Flags& flags) {
    const bool negative = is_negative(a);
    const s64 most_positive = static_cast<s64>((u64{1} << (bits - 1)) - 1);
    const IntegerResult saturated = {negative ? -most_positive - 1 : most_positive, true};
    if (is_infinity(a)) return saturated;
    if (is_zero(a)) return {0, false};

    // Round so the last kept bit has weight 2^0. Huge values can't fit; catch them before shifting.
    const Unpacked value = unpack(a);
    if (value.exponent >= 64) return saturated;
    const Split split = split_off(value.significand, -s64{value.exponent});
    const bool increment = rounds_up(rounding, negative, split);
    const u128 magnitude = split.kept + (increment ? 1 : 0);

    // Negative values reach one further: down to -2^(bits-1).
    const u128 limit = (u128{1} << (bits - 1)) - (negative ? 0 : 1);
    if (magnitude > limit) return saturated;

    flags.inexact |= split.remainder != Remainder::zero;
    flags.fraction_rounded |= increment;
    const u64 low = static_cast<u64>(magnitude);
    return {static_cast<s64>(negative ? 0 - low : low), false};
}

u64 round_to_integral(u64 a, Rounding rounding) {
    if (is_zero(a) || is_infinity(a) || is_nan(a)) return a;
    const Unpacked value = unpack(a);
    if (value.exponent >= 0) return a; // already an integer

    const Split split = split_off(value.significand, -s64{value.exponent});
    const u128 kept = split.kept + (rounds_up(rounding, value.negative, split) ? 1 : 0);
    return pack(value.negative, 0, static_cast<u64>(kept));
}

} // namespace soft_float
