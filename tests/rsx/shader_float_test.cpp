// Shader arithmetic. Expected values are exact IEEE single results (the inputs are chosen so the
// host's float literals are exact too).

#include "rsx/shader_float.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>

using namespace rsx::shader_float;

namespace {

Float f(float value) {
    return std::bit_cast<u32>(value);
}

constexpr Float k_infinity = 0x7f80'0000;
constexpr Float k_negative_zero = 0x8000'0000;

} // namespace

TEST_CASE("shader arithmetic rounds to nearest") {
    CHECK(add(f(1.5f), f(2.25f)) == f(3.75f));
    CHECK(multiply(f(3.0f), f(-0.5f)) == f(-1.5f));
    CHECK(divide(f(1.0f), f(3.0f)) == 0x3eaa'aaab); // 1/3 rounded to nearest
    CHECK(multiply_add(f(2.0f), f(3.0f), f(1.0f)) == f(7.0f));
    CHECK(reciprocal(f(4.0f)) == f(0.25f));
    CHECK(reciprocal_square_root(f(16.0f)) == f(0.25f));
}

TEST_CASE("MAD rounds the product before adding") {
    // (1 + 2^-23) * (1 - 2^-23) = 1 - 2^-46 rounds to 1, so the sum is exactly 0. A fused
    // multiply-add would give -2^-46.
    CHECK(multiply_add(0x3f80'0001, 0x3f7f'fffe, f(-1.0f)) == 0);
}

TEST_CASE("invalid operations give the default NaN") {
    CHECK(add(k_infinity, k_infinity | 0x8000'0000) == k_default_nan);
    CHECK(multiply(k_infinity, k_zero) == k_default_nan);
    CHECK(divide(k_zero, k_zero) == k_default_nan);
    CHECK(add(0x7f80'0001, k_one) == k_default_nan);
    CHECK(reciprocal_square_root(f(-1.0f)) == k_default_nan);
}

TEST_CASE("comparisons treat zeros as equal and NaN as unordered") {
    CHECK(less(f(-2.0f), f(1.0f)));
    CHECK(less(f(-2.0f), f(-1.0f)));
    CHECK_FALSE(less(k_negative_zero, k_zero));
    CHECK(equal(k_negative_zero, k_zero));
    CHECK_FALSE(less(k_default_nan, k_one));
    CHECK_FALSE(equal(k_default_nan, k_default_nan));
    CHECK(minimum(f(-1.0f), f(2.0f)) == f(-1.0f));
    CHECK(maximum(f(-1.0f), f(2.0f)) == f(2.0f));
}

TEST_CASE("floor and integer conversion") {
    CHECK(floor(f(-1.5f)) == f(-2.0f));
    CHECK(floor(f(2.75f)) == f(2.0f));
    CHECK(to_integer(f(2.5f)) == 2); // ties to even
    CHECK(to_integer(f(3.5f)) == 4);
    CHECK(to_integer(f(-0.5f)) == 0);
    CHECK(from_integer(-7) == f(-7.0f));
    CHECK(from_integer((s64{1} << 24) + 1) == f(16777216.0f)); // rounds to nearest even
}
