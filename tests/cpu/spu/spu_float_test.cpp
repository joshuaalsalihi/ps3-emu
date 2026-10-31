// The SPU's extended-range single precision. Expected values follow SPU ISA v1.2, section 9.1.

#include "cpu/spu/spu_float.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <initializer_list>

using namespace spu::spu_float;

namespace {

constexpr u32 k_one = 0x3f80'0000;
constexpr u32 k_two = 0x4000'0000;
constexpr u32 k_half = 0x3f00'0000;
constexpr u32 k_smin = 0x0080'0000;          // the smallest positive number
constexpr u32 k_ieee_infinity = 0x7f80'0000; // 2^128 here: an ordinary number

// The tests compare against the host's IEEE arithmetic, where the values are in IEEE range.
double value_of(u32 x) {
    return std::bit_cast<float>(x);
}

} // namespace

TEST_CASE("extended-range results are truncated") {
    Flags flags;
    // 1 + 1.5 × 2^-24 rounds to nearest as 1 + 2^-23, but truncates to 1.
    CHECK(add(k_one, 0x33c0'0000, flags) == k_one);
    CHECK(add(0xbf80'0000, 0xb3c0'0000, flags) == 0xbf80'0000); // toward zero, not down
    CHECK_FALSE(flags.overflow);
    CHECK_FALSE(flags.underflow);
    CHECK_FALSE(flags.diff);
}

TEST_CASE("exponent 255 is an ordinary number") {
    Flags flags;
    CHECK(add(k_ieee_infinity, k_one, flags) == k_ieee_infinity);
    CHECK(flags.diff); // IEEE would have seen an infinity
    CHECK_FALSE(flags.overflow);
    CHECK(multiply(0x7f00'0000, k_two, flags) == k_ieee_infinity); // 2^127 × 2
}

TEST_CASE("overflow saturates to the largest magnitude, keeping the sign") {
    Flags flags;
    CHECK(add(k_ieee_infinity, k_ieee_infinity, flags) == k_largest);
    CHECK(flags.overflow);

    flags = {};
    CHECK(multiply(0xffff'ffff, k_two, flags) == 0xffff'ffff);
    CHECK(flags.overflow);
}

TEST_CASE("overflow is flagged when truncation brings the result back to Smax") {
    // Smax + 2^81 is above Smax, but truncates to it. Section 9.1: "the magnitude of the
    // result before rounding is bigger than the largest positive representable number".
    Flags flags;
    CHECK(add(k_largest, 0x6800'0000, flags) == k_largest);
    CHECK(flags.overflow);

    flags = {};
    CHECK(add(k_largest, 0, flags) == k_largest); // exactly Smax: no overflow
    CHECK_FALSE(flags.overflow);
}

TEST_CASE("underflow gives positive zero") {
    Flags flags;
    CHECK(multiply(k_smin, k_half, flags) == 0);
    CHECK(flags.underflow);
    CHECK(flags.diff);

    flags = {};
    CHECK(multiply(0x8080'0000, k_half, flags) == 0); // -Smin / 2: +0, not -0
    CHECK(flags.underflow);
}

TEST_CASE("denormal operands are zero and zero results are positive") {
    Flags flags;
    CHECK(add(0x0000'0001, 0, flags) == 0);
    CHECK(flags.diff); // IEEE would have seen a denormal
    CHECK_FALSE(flags.underflow);

    flags = {};
    CHECK(add(0xbf80'0000, k_one, flags) == 0);       // -1 + 1
    CHECK(add(0x8000'0000, 0x8000'0000, flags) == 0); // -0 + -0
    CHECK(multiply(0x8000'0000, k_one, flags) == 0);
}

TEST_CASE("multiply-add rounds once and its product has unlimited range") {
    Flags flags;
    CHECK(multiply_add(k_two, 0x4040'0000, k_one, flags) == 0x40e0'0000); // 2 × 3 + 1 = 7
    // 2^128 × 2 - 2^128 = 2^128, although the product alone would overflow.
    CHECK(multiply_add(k_ieee_infinity, k_two, 0xff80'0000, flags) == k_ieee_infinity);
    CHECK_FALSE(flags.overflow);
}

TEST_CASE("integer conversions truncate and scale") {
    Flags flags;
    CHECK(from_integer(7, 0, flags) == 0x40e0'0000);
    CHECK(from_integer(-1, 0, flags) == 0xbf80'0000);
    CHECK(from_integer(0x7fff'ffff, 0, flags) == 0x4eff'ffff); // nearest would give 2^31
    CHECK(from_integer(0xffff'ffff, 0, flags) == 0x4f7f'ffff); // 2^32 - 256
    CHECK(from_integer(7, 1, flags) == 0x4060'0000);           // 7 / 2
    CHECK(from_integer(0, 0, flags) == 0);
    CHECK_FALSE(flags.underflow);
    CHECK(from_integer(1, 127, flags) == 0); // 2^-127 is below Smin
    CHECK(flags.underflow);

    CHECK(to_integer(0x4070'0000, 0, 32) == 3);  // 3.75
    CHECK(to_integer(0xc070'0000, 0, 32) == -3); // -3.75
    CHECK(to_integer(0x3fc0'0000, 1, 32) == 3);  // 1.5 × 2
    CHECK(to_integer(k_ieee_infinity, 0, 32) == 0x7fff'ffff);
    CHECK(to_integer(0xff80'0000, 0, 32) == -s64{0x8000'0000});
    CHECK(to_integer(0x0000'0001, 100, 32) == 0); // a denormal is zero
}

TEST_CASE("compares treat every zero exponent as zero") {
    CHECK(equal(0, 0x8000'0000));
    CHECK(equal(0x0000'0001, 0x8040'0000));
    CHECK_FALSE(equal(k_one, k_two));
    CHECK(greater(k_largest, k_ieee_infinity));
    CHECK(greater(0xbf80'0000, 0xc000'0000)); // -1 > -2
    CHECK_FALSE(greater(0, 0x8000'0001));
}

TEST_CASE("reciprocal estimates, interpolated, are within 2^-10") {
    // UNVERIFIED: our own estimate tables. The measured worst case is 2^-10.46.
    for (const u32 exponent : {1u, 100u, 127u, 200u, 252u}) {
        for (u32 fraction = 0; fraction < 0x80'0000; fraction += 0x1357) {
            for (const u32 sign : {0u, 0x8000'0000u}) {
                const u32 x = sign | (exponent << 23) | fraction;
                Flags flags;
                const u32 y = interpolate(x, reciprocal_estimate(x, flags), flags);
                INFO(x);
                REQUIRE(std::fabs(value_of(y) * value_of(x) - 1) < 0x1p-10);
            }
        }
    }
}

TEST_CASE("reciprocal square root estimates, interpolated, are within 2^-11") {
    // UNVERIFIED: our own estimate tables. The measured worst case is 2^-11.77.
    for (const u32 exponent : {1u, 2u, 127u, 128u, 254u}) {
        for (u32 fraction = 0; fraction < 0x80'0000; fraction += 0x1357) {
            const u32 x = (exponent << 23) | fraction;
            Flags flags;
            const u32 y = interpolate(x, reciprocal_square_root_estimate(x, flags), flags);
            INFO(x);
            REQUIRE(std::fabs(value_of(y) * std::sqrt(value_of(x)) - 1) < 0x1p-11);
        }
    }
}

TEST_CASE("estimates of zero flag a divide by zero and give the largest number") {
    Flags flags;
    CHECK(reciprocal_estimate(0x8000'0000, flags) == 0xffff'ffff);
    CHECK(flags.divide_by_zero);
    flags = {};
    CHECK(reciprocal_square_root_estimate(0x0000'0001, flags) == k_largest);
    CHECK(flags.divide_by_zero);
}

TEST_CASE("reciprocals of 2^126 and above underflow to zero") {
    // Section 9, "Floating Reciprocal Estimate": "If |x| >= 2^126, then 1/x underflows".
    Flags flags;
    CHECK(reciprocal_estimate(0x7e80'0000, flags) == 0);
    CHECK(reciprocal_estimate(0x7fff'ffff, flags) == 0);
    // Just below 2^126 the result is still representable.
    CHECK(interpolate(0x7e7f'ffff, reciprocal_estimate(0x7e7f'ffff, flags), flags) != 0);
    CHECK_FALSE(flags.divide_by_zero);
}
