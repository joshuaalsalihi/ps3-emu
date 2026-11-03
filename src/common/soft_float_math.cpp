#include "common/soft_float_math.h"

#include "common/soft_float.h"

namespace soft_float {

namespace {

constexpr u64 k_one = 0x3ff0'0000'0000'0000;

} // namespace

// The exponent plus log2 of the significand m, whose bits come one at a time from repeated
// squaring: if m² ≥ 2 the next bit is 1, and m is halved.
u64 log2(u64 x) {
    Flags flags;
    const s64 exponent = static_cast<s64>((x >> 52) & 0x7ff) - 1023;
    u64 significand = (x & ((u64{1} << 52) - 1)) | k_one; // in [1, 2)

    s64 fraction = 0;
    for (int bit = 0; bit < 24; ++bit) {
        significand = multiply(significand, significand, k_double, {}, flags);
        fraction <<= 1;
        if (((significand >> 52) & 0x7ff) >= 1024) { // ≥ 2
            fraction |= 1;
            significand -= u64{1} << 52; // halve
        }
    }
    const u64 scaled = from_integer(exponent * (s64{1} << 24) + fraction, k_double, {}, flags);
    return is_zero(scaled) ? 0 : scaled - (u64{24} << 52); // ÷ 2^24, exactly
}

// The product of the factors 2^(2^-k) for each set bit k of f. Each factor is 2 square-rooted
// k times.
u64 exp2_fraction(u64 f) {
    Flags flags;
    const s64 bits = to_integer(is_zero(f) ? 0 : f + (u64{24} << 52), 64, Rounding::toward_zero,
                                flags)
                         .value; // the top 24 bits of f
    u64 result = k_one;
    u64 factor = 0x4000'0000'0000'0000; // 2.0
    for (int k = 1; k <= 24; ++k) {
        factor = square_root(factor, k_double, {}, flags); // 2^(2^-k)
        if (bits & (s64{1} << (24 - k))) result = multiply(result, factor, k_double, {}, flags);
    }
    return result;
}

} // namespace soft_float
