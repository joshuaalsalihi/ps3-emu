// Floating-point instructions: results, NaN rules, exceptions and FPSCR updates.
// Expected values are worked out from Power ISA v3.1, Book I, chapter 4.

#include "cpu/ppu/fpscr.h"
#include "support/ppu_test.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

using namespace ppu;
using namespace ppu::fpscr;
using test::PpuTest;

namespace {

struct FloatTest : PpuTest {
    void set(u32 n, double value) { state().fpr[n] = std::bit_cast<u64>(value); }
    double value(u32 n) { return std::bit_cast<double>(state().fpr[n]); }
    u64& bits(u32 n) { return state().fpr[n]; }
    u32 fpscr() { return state().fpscr; }
    bool has(u32 flags) { return (fpscr() & flags) == flags; }
    u32 fprf() { return fpscr() & k_fprf_mask; }
};

constexpr u64 k_quiet_nan = 0x7ff8'0000'0000'0000;
constexpr u64 k_nan_with_payload = 0x7ff8'0000'0000'1234;
constexpr u64 k_signaling_nan = 0x7ff0'0000'0000'5678;
constexpr u64 k_infinity = 0x7ff0'0000'0000'0000;
constexpr u64 k_largest = 0x7fef'ffff'ffff'ffff;

} // namespace

// ---- Arithmetic and result flags ----

TEST_CASE_METHOD(FloatTest, "fadd computes the sum and classifies it") {
    set(2, 1.5);
    set(3, 2.25);
    run({"fadd f1, f2, f3"});
    CHECK(value(1) == 3.75);
    CHECK(fprf() == k_fg); // positive normal
    CHECK_FALSE(has(k_fi));
    CHECK_FALSE(has(k_xx));
}

TEST_CASE_METHOD(FloatTest, "inexact results set FI and XX; FR when rounding increments") {
    set(2, 1.0);
    set(3, std::ldexp(1.0, -60));

    SECTION("round to nearest") {
        run({"fadd f1, f2, f3"});
        CHECK(value(1) == 1.0);
        CHECK(has(k_fi | k_xx | k_fx));
        CHECK_FALSE(has(k_fr));
    }
    SECTION("round toward +infinity") {
        run({"mtfsfi cr7, 2", "fadd f1, f2, f3"});
        CHECK(value(1) == std::nextafter(1.0, 2.0));
        CHECK(has(k_fr | k_fi));
    }
}

TEST_CASE_METHOD(FloatTest, "single-precision instructions round to single") {
    set(2, 1.0);
    set(3, std::ldexp(1.0, -30)); // representable in single, but lost in a single-precision sum
    run({"fadds f1, f2, f3", "fadd f4, f2, f3"});
    CHECK(value(1) == 1.0);
    CHECK(value(4) == 1.0 + std::ldexp(1.0, -30));
}

TEST_CASE_METHOD(FloatTest, "single-precision denormals are classified as denormal") {
    set(2, std::ldexp(1.0, -126));
    set(3, 0.5);
    run({"fmuls f1, f2, f3"}); // 2^-127: a normal double, but a single-precision denormal
    CHECK(value(1) == std::ldexp(1.0, -127));
    CHECK(fprf() == (k_c | k_fg));
}

TEST_CASE_METHOD(FloatTest, "the record forms copy FX, FEX, VX and OX into CR1") {
    set(2, 1.0);
    set(3, std::ldexp(1.0, -60));
    run({"fadd. f1, f2, f3"});
    CHECK(cr_field(1) == 0b1000); // FX
}

// ---- NaNs ----

TEST_CASE_METHOD(FloatTest, "the result NaN is FRA's, then FRB's, then FRC's") {
    bits(2) = k_nan_with_payload;
    bits(3) = k_quiet_nan;
    bits(4) = 0x7ff8'0000'0000'9999;
    set(5, 1.0);
    run({"fadd f1, f2, f3", "fadd f6, f5, f3", "fmadd f7, f5, f4, f5", "fmul f8, f5, f4"});
    CHECK(bits(1) == k_nan_with_payload);
    CHECK(bits(6) == k_quiet_nan);
    CHECK(bits(7) == 0x7ff8'0000'0000'9999);
    CHECK(bits(8) == 0x7ff8'0000'0000'9999); // fmul's operands are FRA and FRC
    CHECK(fprf() == (k_c | k_fu));
    CHECK_FALSE(has(k_vx)); // quiet NaNs raise nothing
}

TEST_CASE_METHOD(FloatTest, "a signaling NaN is quieted and raises VXSNAN") {
    bits(2) = k_signaling_nan;
    set(3, 1.0);
    run({"fadd f1, f2, f3"});
    CHECK(bits(1) == (k_signaling_nan | (u64{1} << 51)));
    CHECK(has(k_vxsnan | k_vx | k_fx));
    CHECK_FALSE(has(k_fex)); // not enabled
}

TEST_CASE_METHOD(FloatTest, "an enabled invalid operation leaves the target unchanged") {
    bits(2) = k_signaling_nan;
    set(3, 1.0);
    set(1, 42.0);
    run({"mtfsb1 24", "fadd f1, f2, f3"}); // bit 24: VE
    CHECK(value(1) == 42.0);
    CHECK(has(k_vxsnan | k_fex));
}

// ---- Invalid operations ----

TEST_CASE_METHOD(FloatTest, "invalid operations give the default NaN") {
    SECTION("infinity - infinity") {
        bits(2) = k_infinity;
        bits(3) = k_infinity;
        run({"fsub f1, f2, f3"});
        CHECK(bits(1) == k_quiet_nan);
        CHECK(has(k_vxisi | k_vx));
    }
    SECTION("infinity × 0") {
        bits(2) = k_infinity;
        set(3, 0.0);
        run({"fmul f1, f2, f3"});
        CHECK(bits(1) == k_quiet_nan);
        CHECK(has(k_vximz));
    }
    SECTION("0 / 0 and infinity / infinity") {
        set(2, 0.0);
        bits(3) = k_infinity;
        run({"fdiv f1, f2, f2", "fdiv f4, f3, f3"});
        CHECK(bits(1) == k_quiet_nan);
        CHECK(bits(4) == k_quiet_nan);
        CHECK(has(k_vxzdz | k_vxidi));
    }
    SECTION("square root of a negative number") {
        set(2, -1.0);
        set(3, -0.0);
        run({"fsqrt f1, f2", "fsqrt f4, f3"});
        CHECK(bits(1) == k_quiet_nan);
        CHECK(std::signbit(value(4))); // sqrt(-0) = -0, and is valid
        CHECK(has(k_vxsqrt));
    }
}

TEST_CASE_METHOD(FloatTest, "infinity × 0 is invalid even when the addend is a quiet NaN") {
    bits(2) = k_infinity;
    set(4, 0.0);
    bits(3) = k_nan_with_payload;
    run({"fmadd f1, f2, f4, f3"});
    CHECK(bits(1) == k_nan_with_payload);
    CHECK(has(k_vximz));
}

// ---- Zero divide, overflow, underflow ----

TEST_CASE_METHOD(FloatTest, "dividing by zero gives infinity and ZX") {
    set(2, -3.0);
    set(3, 0.0);
    run({"fdiv f1, f2, f3"});
    CHECK(bits(1) == (k_infinity | soft_float::k_sign_bit));
    CHECK(has(k_zx));
    CHECK(fprf() == (k_fl | k_fu)); // negative infinity
}

TEST_CASE_METHOD(FloatTest, "an enabled zero divide leaves the target unchanged") {
    set(2, 3.0);
    set(3, 0.0);
    set(1, 7.0);
    run({"mtfsb1 27", "fdiv f1, f2, f3"}); // bit 27: ZE
    CHECK(value(1) == 7.0);
    CHECK(has(k_zx | k_fex));
}

TEST_CASE_METHOD(FloatTest, "overflow: infinity when disabled, a wrapped exponent when enabled") {
    bits(2) = k_largest;
    set(3, 2.0);
    SECTION("disabled") {
        run({"fmul f1, f2, f3"});
        CHECK(bits(1) == k_infinity);
        CHECK(has(k_ox | k_xx | k_fi));
    }
    SECTION("enabled: the exponent drops by 1536") {
        run({"mtfsb1 25", "fmul f1, f2, f3"}); // bit 25: OE
        CHECK(bits(1) == 0x1fff'ffff'ffff'ffff);
        CHECK(has(k_ox | k_fex));
        CHECK_FALSE(has(k_xx));
    }
}

TEST_CASE_METHOD(FloatTest, "underflow is detected before rounding") {
    // (1 - 2^-53) × 2^-1022 is tiny, but rounds up to the smallest normal number.
    bits(2) = 0x3fef'ffff'ffff'ffff;
    bits(3) = 0x0010'0000'0000'0000;
    run({"fmul f1, f2, f3"});
    CHECK(bits(1) == 0x0010'0000'0000'0000);
    CHECK(has(k_ux | k_xx | k_fr | k_fi));
}

// ---- Fused multiply-add ----

TEST_CASE_METHOD(FloatTest, "multiply-add rounds once") {
    // (1 + 2^-30)(1 - 2^-30) - 1 = -2^-60 exactly; rounding the product first would give 0.
    set(2, 1.0 + std::ldexp(1.0, -30));
    set(4, 1.0 - std::ldexp(1.0, -30));
    set(3, 1.0);
    run({"fmsub f1, f2, f4, f3", "fmul f5, f2, f4", "fsub f6, f5, f3"});
    CHECK(value(1) == -std::ldexp(1.0, -60));
    CHECK(value(6) == 0.0);
}

TEST_CASE_METHOD(FloatTest, "negative multiply-add negates after rounding, but not NaNs") {
    set(2, 2.0);
    set(4, 3.0);
    set(3, 1.0);
    bits(5) = k_nan_with_payload;
    run({"fnmadd f1, f2, f4, f3", "fnmsub f6, f2, f4, f3", "fnmadd f7, f5, f4, f3"});
    CHECK(value(1) == -7.0);
    CHECK(value(6) == -5.0);
    CHECK(bits(7) == k_nan_with_payload);
}

// ---- Estimates, square root ----

TEST_CASE_METHOD(FloatTest, "square root and estimates") {
    set(2, 4.0);
    set(3, 0.0);
    run({"fsqrt f1, f2", "fres f4, f2", "frsqrte f5, f2", "fres f6, f3"});
    CHECK(value(1) == 2.0);
    CHECK(value(4) == 0.25);
    CHECK(value(5) == 0.5);
    CHECK(bits(6) == k_infinity);
    CHECK(has(k_zx));
}

// ---- Rounding and conversion ----

TEST_CASE_METHOD(FloatTest, "frsp rounds to single and trims NaN payloads") {
    set(2, 1.0 / 3.0);
    run({"frsp f1, f2"});
    CHECK(value(1) == static_cast<double>(1.0f / 3.0f));
    CHECK(has(k_fi));

    bits(3) = 0x7ff8'0000'0000'0fff;
    run({"frsp f4, f3"});
    CHECK(bits(4) == k_quiet_nan); // low 29 fraction bits cleared
    CHECK_FALSE(has(k_fi));        // NaN results clear FR and FI
}

TEST_CASE_METHOD(FloatTest, "conversion to integer rounds by RN, or truncates") {
    set(2, 2.5);
    set(3, -2.7);
    run({"fctiw f1, f2", "fctiwz f4, f3", "fctid f5, f3"});
    CHECK(bits(1) == 2);           // ties to even
    CHECK(bits(4) == 0xffff'fffe); // -2, in the low word
    CHECK(bits(5) == static_cast<u64>(-3));
    CHECK(has(k_fi | k_xx));
}

TEST_CASE_METHOD(FloatTest, "out-of-range and NaN conversions saturate and raise VXCVI") {
    set(2, 3e9);
    bits(3) = k_signaling_nan;
    run({"fctiw f1, f2", "fctid f4, f3"});
    CHECK(bits(1) == 0x7fff'ffff);
    CHECK(bits(4) == 0x8000'0000'0000'0000);
    CHECK(has(k_vxcvi | k_vxsnan));
}

TEST_CASE_METHOD(FloatTest, "fcfid converts a 64-bit integer, rounding if needed") {
    bits(2) = (u64{1} << 53) + 1;
    bits(3) = static_cast<u64>(-5);
    run({"fcfid f1, f2", "fcfid f4, f3"});
    CHECK(value(1) == std::ldexp(1.0, 53)); // ties to even
    CHECK(value(4) == -5.0);
    CHECK(has(k_xx));
}

// ---- Compare ----

TEST_CASE_METHOD(FloatTest, "compares set a CR field and FPCC") {
    set(2, 1.0);
    set(3, 2.0);
    set(4, -0.0);
    set(5, 0.0);
    bits(6) = k_quiet_nan;
    run({"fcmpu cr1, f2, f3", "fcmpu cr2, f3, f2", "fcmpu cr3, f4, f5", "fcmpu cr4, f2, f6"});
    CHECK(cr_field(1) == k_cr_lt);
    CHECK(cr_field(2) == k_cr_gt);
    CHECK(cr_field(3) == k_cr_eq); // -0 == +0
    CHECK(cr_field(4) == k_cr_so); // unordered
    CHECK((fpscr() & k_fpcc_mask) == k_fu);
    CHECK_FALSE(has(k_vx)); // fcmpu doesn't mind quiet NaNs
}

TEST_CASE_METHOD(FloatTest, "ordered compares treat NaNs as invalid") {
    bits(2) = k_quiet_nan;
    bits(3) = k_signaling_nan;
    set(4, 1.0);
    SECTION("quiet NaN") {
        run({"fcmpo cr1, f2, f4"});
        CHECK(has(k_vxvc));
    }
    SECTION("signaling NaN, VE = 0: both VXSNAN and VXVC") {
        run({"fcmpo cr1, f3, f4"});
        CHECK(has(k_vxsnan | k_vxvc));
    }
    SECTION("signaling NaN, VE = 1: only VXSNAN") {
        run({"mtfsb1 24", "fcmpo cr1, f3, f4"});
        CHECK(has(k_vxsnan));
        CHECK_FALSE(has(k_vxvc));
    }
}

// ---- Moves and select ----

TEST_CASE_METHOD(FloatTest, "moves change only the sign bit, even of NaNs") {
    bits(2) = k_nan_with_payload;
    set(3, -1.5);
    run({"fmr f1, f2", "fneg f4, f2", "fabs f5, f3", "fnabs f6, f5"});
    CHECK(bits(1) == k_nan_with_payload);
    CHECK(bits(4) == (k_nan_with_payload | soft_float::k_sign_bit));
    CHECK(value(5) == 1.5);
    CHECK(value(6) == -1.5);
    CHECK(fpscr() == 0);
}

TEST_CASE_METHOD(FloatTest, "fsel picks FRC when FRA >= 0, including -0") {
    set(3, 10.0); // FRB
    set(4, 20.0); // FRC
    set(2, -0.0);
    set(5, -1.0);
    bits(6) = k_quiet_nan;
    run({"fsel f1, f2, f4, f3", "fsel f7, f5, f4, f3", "fsel f8, f6, f4, f3"});
    CHECK(value(1) == 20.0);
    CHECK(value(7) == 10.0);
    CHECK(value(8) == 10.0);
}

// ---- FPSCR instructions ----

TEST_CASE_METHOD(FloatTest, "moving to and from the FPSCR") {
    SECTION("mtfsfi sets a field; mffs reads the register") {
        run({"mtfsfi cr7, 3", "mffs f1"});
        CHECK(bits(1) == 3);
        CHECK((fpscr() & k_rn_mask) == 3);
    }
    SECTION("mtfsb1 on an exception bit also sets FX") {
        run({"mtfsb1 3"}); // OX
        CHECK(has(k_ox | k_fx));
    }
    SECTION("FEX and VX can't be set directly") {
        run({"mtfsb1 1", "mtfsb1 2"});
        CHECK(fpscr() == 0);
    }
    SECTION("mtfsf copies fields but recomputes the summaries") {
        bits(2) = 0xffff'ffff;
        run({"mtfsf 0x80, f2"}); // field 0 only: FX, FEX, VX, OX
        CHECK(has(k_fx | k_ox));
        CHECK_FALSE(has(k_vx)); // no invalid bits are set
    }
    SECTION("mcrfs copies a field and clears its exception bits") {
        run({"mtfsb1 3", "mcrfs cr2, cr0", "mffs f1"});
        CHECK(cr_field(2) == 0b1001); // FX and OX
        CHECK(bits(1) == 0);          // both cleared
    }
}

TEST_CASE_METHOD(FloatTest, "stfiwx stores the low word unchanged") {
    bits(1) = 0x1234'5678'9abc'def0;
    r(4) = k_data;
    run({"stfiwx f1, r0, r4"});
    CHECK(memory.read<u32>(k_data) == 0x9abc'def0);
}
