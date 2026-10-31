#include "support/spu_test.h"

#include "cpu/spu/spu_float.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

using namespace spu;
using test::SpuTest;

namespace {

constexpr u32 k_one = 0x3f80'0000;
constexpr u32 k_two = 0x4000'0000;
constexpr u32 k_three = 0x4040'0000;

constexpr u64 k_one_double = 0x3ff0'0000'0000'0000;
constexpr u64 k_two_double = 0x4000'0000'0000'0000;
constexpr u64 k_three_double = 0x4008'0000'0000'0000;
constexpr u64 k_infinity = 0x7ff0'0000'0000'0000;
constexpr u64 k_default_nan = 0x7ff8'0000'0000'0000;

u32 single_flag(u32 slot, u32 flag) {
    return k_fpscr_single_flags[slot] + flag;
}
u32 double_flag(u32 slot, u32 flag) {
    return k_fpscr_double_flags[slot] + flag;
}

} // namespace

// ---- Single precision ----

TEST_CASE_METHOD(SpuTest, "single-precision arithmetic on each word") {
    set_words(1, k_one, k_two, 0xbf80'0000, 0x3fc0'0000);   // 1, 2, -1, 1.5
    set_words(2, k_two, k_three, 0xbf80'0000, 0x4010'0000); // 2, 3, -1, 2.25
    set_all(3, k_one);
    run({"fa $4, $1, $2", "fs $5, $1, $2", "fm $6, $1, $2", "fma $7, $1, $2, $3",
         "fms $8, $1, $2, $3", "fnms $9, $1, $2, $3"});
    CHECK(r(4) == Vector::from_words(0x4040'0000, 0x40a0'0000, 0xc000'0000, 0x4070'0000));
    CHECK(r(5) == Vector::from_words(0xbf80'0000, 0xbf80'0000, 0, 0xbf40'0000)); // -1 - -1 = +0
    CHECK(r(6) == Vector::from_words(k_two, 0x40c0'0000, k_one, 0x4058'0000));
    CHECK(r(7).word(1) == 0x40e0'0000); // 2 × 3 + 1 = 7
    CHECK(r(8).word(1) == 0x40a0'0000); // 2 × 3 - 1 = 5
    CHECK(r(9).word(1) == 0xc0a0'0000); // 1 - 2 × 3 = -5
}

TEST_CASE_METHOD(SpuTest, "single-precision flags go to the slot that raised them") {
    set_words(1, k_one, 0x0080'0000, 0x7f80'0000, k_one);
    set_words(2, k_one, 0x3f00'0000, 0x7f80'0000, k_one);
    run({"fm $3, $1, $2"}); // slot 1 underflows, slot 2 overflows
    CHECK(r(3) == Vector::from_words(k_one, 0, spu_float::k_largest, k_one));
    CHECK_FALSE(fpscr_bit(single_flag(0, k_single_diff)));
    CHECK(fpscr_bit(single_flag(1, k_single_underflow)));
    CHECK(fpscr_bit(single_flag(1, k_single_diff)));
    CHECK(fpscr_bit(single_flag(2, k_single_overflow)));
    CHECK(fpscr_bit(single_flag(2, k_single_diff)));
    CHECK_FALSE(fpscr_bit(single_flag(3, k_single_overflow)));
}

TEST_CASE_METHOD(SpuTest, "the estimate sequences from the manual converge") {
    // SPU ISA, "Floating Reciprocal Estimate" and "Floating Reciprocal Absolute Square Root
    // Estimate". UNVERIFIED: the hardware's tables reach 1 ulp; our measured worst cases are
    // 2^-20.75 and 2^-22.26.
    const float x = 3.14159f;
    set_all(1, std::bit_cast<u32>(x));
    set_all(10, k_one);
    set_all(11, 0x3f00'0000); // 0.5
    run({"frest $2, $1", "fi $3, $1, $2", "fnms $4, $1, $3, $10", "fma $5, $4, $3, $3",
         "frsqest $6, $1", "fi $7, $1, $6", "fm $8, $1, $7", "fm $9, $7, $11",
         "fnms $8, $8, $7, $10", "fma $12, $8, $9, $7"});
    CHECK(std::fabs(std::bit_cast<float>(preferred(5)) * double{x} - 1) < 0x1p-20);
    CHECK(std::fabs(std::bit_cast<float>(preferred(12)) * std::sqrt(double{x}) - 1) < 0x1p-21);
}

TEST_CASE_METHOD(SpuTest, "estimates of zero set the divide-by-zero flag") {
    set_words(1, k_one, 0, k_one, 0x8000'0001);
    run({"frest $2, $1"});
    CHECK_FALSE(fpscr_bit(k_fpscr_divide_by_zero + 0));
    CHECK(fpscr_bit(k_fpscr_divide_by_zero + 1));
    CHECK(fpscr_bit(k_fpscr_divide_by_zero + 3));
    CHECK(r(2).word(3) == 0xffff'ffff);
}

TEST_CASE_METHOD(SpuTest, "integer conversions use the encoded scale") {
    set_words(1, 7, static_cast<u32>(-1), 0x7fff'ffff, 0xffff'ffff);
    run({"csflt $2, $1, 0", "csflt $3, $1, 1", "cuflt $4, $1, 0"});
    CHECK(r(2) == Vector::from_words(0x40e0'0000, 0xbf80'0000, 0x4eff'ffff, 0xbf80'0000));
    CHECK(r(3).word(0) == 0x4060'0000); // 7 / 2
    CHECK(r(4).word(1) == 0x4f7f'ffff); // 0xffffffff, truncated
    CHECK(r(4).word(0) == 0x40e0'0000);

    set_words(5, 0x4070'0000, 0xc070'0000, 0x7f80'0000, 0x3fc0'0000); // 3.75, -3.75, 2^128, 1.5
    run({"cflts $6, $5, 0", "cfltu $7, $5, 0", "cflts $8, $5, 1"});
    CHECK(r(6) == Vector::from_words(3, static_cast<u32>(-3), 0x7fff'ffff, 1));
    CHECK(r(7) == Vector::from_words(3, 0, 0xffff'ffff, 1)); // negatives saturate to 0
    CHECK(r(8).word(3) == 3);                                // 1.5 × 2
}

TEST_CASE_METHOD(SpuTest, "single-precision compares") {
    set_words(1, 0, 0x0000'0001, 0xbf80'0000, 0xc000'0000); // 0, denormal, -1, -2
    set_words(2, 0x8000'0000, 0, k_one, 0xbf80'0000);       // -0, 0, 1, -1
    run({"fceq $3, $1, $2", "fcmeq $4, $1, $2", "fcgt $5, $1, $2", "fcmgt $6, $1, $2"});
    CHECK(r(3) == Vector::from_words(~0u, ~0u, 0, 0));
    CHECK(r(4) == Vector::from_words(~0u, ~0u, ~0u, 0));
    CHECK(r(5) == Vector::from_words(0, 0, 0, 0));
    CHECK(r(6) == Vector::from_words(0, 0, 0, ~0u)); // |-2| > |-1|
}

// ---- Double precision ----

TEST_CASE_METHOD(SpuTest, "double-precision arithmetic") {
    r(1) = Vector::from_dwords(k_two_double, k_one_double);
    r(2) = Vector::from_dwords(k_three_double, k_two_double);
    r(3) = Vector::from_dwords(k_one_double, k_one_double);
    r(4) = r(3);
    r(5) = r(3);
    r(6) = r(3);
    run({"dfa $10, $1, $2", "dfs $11, $1, $2", "dfm $12, $1, $2", "dfma $3, $1, $2",
         "dfms $4, $1, $2", "dfnms $5, $1, $2", "dfnma $6, $1, $2"});
    CHECK(r(10) == Vector::from_dwords(0x4014'0000'0000'0000, k_three_double)); // 5, 3
    CHECK(r(11) == Vector::from_dwords(0xbff0'0000'0000'0000, 0xbff0'0000'0000'0000));
    CHECK(r(12) == Vector::from_dwords(0x4018'0000'0000'0000, k_two_double)); // 6, 2
    CHECK(r(3).dword(0) == 0x401c'0000'0000'0000);                            // 2 × 3 + 1 = 7
    CHECK(r(4).dword(0) == 0x4014'0000'0000'0000);                            // 2 × 3 - 1 = 5
    CHECK(r(5).dword(0) == 0xc014'0000'0000'0000);                            // -(2 × 3 - 1)
    CHECK(r(6).dword(0) == 0xc01c'0000'0000'0000);                            // -(2 × 3 + 1)
}

TEST_CASE_METHOD(SpuTest, "each doubleword slot has its own rounding mode") {
    set_words(1, 0x0000'0200, 0, 0, 0); // RN0 = nearest, RN1 = toward +infinity
    r(2) = Vector::from_dwords(k_one_double, k_one_double);
    r(3) = Vector::from_dwords(0x3c30'0000'0000'0000, 0x3c30'0000'0000'0000); // 2^-60
    run({"fscrwr $1", "dfa $4, $2, $3"});
    CHECK(r(4) == Vector::from_dwords(k_one_double, 0x3ff0'0000'0000'0001));
    CHECK(fpscr_bit(double_flag(0, k_double_inexact)));
    CHECK(fpscr_bit(double_flag(1, k_double_inexact)));
}

TEST_CASE_METHOD(SpuTest, "double-precision exceptions set sticky flags") {
    r(1) = Vector::from_dwords(0x7fe0'0000'0000'0000, k_infinity); // 2^1023, infinity
    r(2) = Vector::from_dwords(k_two_double, k_infinity);
    run({"dfm $3, $1, $2", "dfs $4, $1, $2"});
    CHECK(r(3).dword(0) == k_infinity);
    CHECK(fpscr_bit(double_flag(0, k_double_overflow)));
    CHECK(fpscr_bit(double_flag(0, k_double_inexact)));
    CHECK(r(4).dword(1) == k_default_nan); // infinity - infinity
    CHECK(fpscr_bit(double_flag(1, k_double_invalid)));
    CHECK_FALSE(fpscr_bit(double_flag(1, k_double_nan))); // no NaN operand
}

TEST_CASE_METHOD(SpuTest, "NaN operands give the default NaN") {
    // UNVERIFIED: whether the Cell propagates NaN operands; the manual allows the default NaN.
    r(1) = Vector::from_dwords(0xfff8'0000'0000'1234, 0x7ff0'0000'0000'0001); // QNaN, SNaN
    r(2) = Vector::from_dwords(k_one_double, k_one_double);
    r(3) = r(2);
    run({"dfa $4, $1, $2", "dfnma $3, $1, $2"});
    CHECK(r(4) == Vector::from_dwords(k_default_nan, k_default_nan));
    CHECK(r(3) == Vector::from_dwords(k_default_nan, k_default_nan)); // not negated
    CHECK(fpscr_bit(double_flag(0, k_double_nan)));
    CHECK_FALSE(fpscr_bit(double_flag(0, k_double_invalid)));
    CHECK(fpscr_bit(double_flag(1, k_double_nan)));
    CHECK(fpscr_bit(double_flag(1, k_double_invalid))); // signaling
}

TEST_CASE_METHOD(SpuTest, "denormal operands become zero and set DENORM") {
    // UNVERIFIED: whether the Cell's SPU forces denormal operands to zero.
    r(1) = Vector::from_dwords(0x0000'0000'0000'0001, 0x8000'0000'0000'0001);
    r(2) = Vector::from_dwords(0, k_one_double);
    run({"dfa $3, $1, $2", "dfm $4, $1, $2"});
    CHECK(r(3) == Vector::from_dwords(0, k_one_double));
    CHECK(r(4).dword(1) == 0x8000'0000'0000'0000); // the sign is kept
    CHECK(fpscr_bit(double_flag(0, k_double_denorm)));
    CHECK(fpscr_bit(double_flag(1, k_double_denorm)));
    CHECK_FALSE(fpscr_bit(double_flag(1, k_double_inexact)));
}

TEST_CASE_METHOD(SpuTest, "denormal double results are produced") {
    r(1) = Vector::from_dwords(0x0010'0000'0000'0000, 0); // 2^-1022
    r(2) = Vector::from_dwords(0x3fe0'0000'0000'0000, 0); // 0.5
    run({"dfm $3, $1, $2"});
    CHECK(r(3).dword(0) == 0x0008'0000'0000'0000); // exact, so no underflow
    CHECK_FALSE(fpscr_bit(double_flag(0, k_double_underflow)));
}

TEST_CASE_METHOD(SpuTest, "frds rounds each doubleword into its left word") {
    r(1) = Vector::from_dwords(0x3fb9'9999'9999'999a, 0x7e37'e43c'8800'759c); // 0.1, 1e300
    run({"frds $2, $1"});
    CHECK(r(2) == Vector::from_words(0x3dcc'cccd, 0, 0x7f80'0000, 0));
    CHECK(fpscr_bit(double_flag(0, k_double_inexact)));
    CHECK(fpscr_bit(double_flag(1, k_double_overflow)));
}

TEST_CASE_METHOD(SpuTest, "fesd extends the left word of each doubleword") {
    set_words(1, 0x3fc0'0000, 0x1234'5678, 0x7f80'0001, 0); // 1.5, ignored, SNaN, ignored
    set_words(2, 0x8000'0001, 0, 0x7f80'0000, 0);           // -denormal, infinity
    run({"fesd $3, $1", "fesd $4, $2"});
    CHECK(r(3) == Vector::from_dwords(0x3ff8'0000'0000'0000, k_default_nan));
    CHECK(fpscr_bit(double_flag(1, k_double_invalid)));
    CHECK(fpscr_bit(double_flag(1, k_double_nan)));
    CHECK(r(4) == Vector::from_dwords(0x8000'0000'0000'0000, k_infinity));
    CHECK(fpscr_bit(double_flag(0, k_double_denorm)));
}

// ---- FPSCR ----

TEST_CASE_METHOD(SpuTest, "the FPSCR keeps only its defined bits") {
    set_all(1, 0xffff'ffff);
    run({"fscrwr $1", "fscrrd $2"});
    // Rounding (20-23) and slot 0's single flags (29-31); then each slot's flags, and the
    // divide-by-zero bits (116-119). SPU ISA v1.2, section 9.3.
    CHECK(r(2) == Vector::from_words(0x0000'0f07, 0x0000'3f07, 0x0000'3f07, 0x0000'0f07));

    set_all(1, 0);
    run({"fscrwr $1", "fscrrd $2"});
    CHECK(r(2) == Vector{});
}

TEST_CASE_METHOD(SpuTest, "FPSCR flags are sticky") {
    set_all(1, 0x7f80'0000);
    set_all(2, k_one);
    run({"fa $3, $1, $1", "fa $3, $2, $2", "fscrrd $4"});
    CHECK(fpscr_bit(single_flag(0, k_single_overflow)));
    CHECK(r(4).word(0) == 0x0000'0005); // OVF and DIFF of slot 0
}
