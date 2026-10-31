// Vector (VMX) instructions. Expected values are worked out from the Power ISA v3.1 pseudocode
// and, for graphics rounding mode, the Cell BE Programming Handbook (v1.11, A.3.3).

#include "support/ppu_test.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

using namespace ppu;
using test::PpuTest;

namespace {

struct VectorTest : PpuTest {
    Vector& v(u32 n) { return state().vr[n]; }
    void words(u32 n, u32 a, u32 b, u32 c, u32 d) { v(n) = Vector::from_words(a, b, c, d); }
    void all_words(u32 n, u32 value) { words(n, value, value, value, value); }
    void floats(u32 n, float a, float b, float c, float d) {
        words(n, std::bit_cast<u32>(a), std::bit_cast<u32>(b), std::bit_cast<u32>(c),
              std::bit_cast<u32>(d));
    }
    float f(u32 n, u32 i) { return std::bit_cast<float>(v(n).word(i)); }
    bool saturated() { return (state().vscr & k_vscr_saturated) != 0; }
    void bytes(u32 n, std::initializer_list<u8> values) {
        u32 i = 0;
        for (const u8 value : values) {
            v(n).set_byte(i++, value);
        }
    }
    void ieee_mode(bool non_java) {
        thread.vmx_graphics_mode = false;
        state().vscr = non_java ? k_vscr_non_java : 0;
    }
};

} // namespace

// ---- Integer arithmetic ----

TEST_CASE_METHOD(VectorTest, "modulo adds wrap, saturating adds clamp and set SAT") {
    all_words(2, 0xf0f0'7f80);
    all_words(3, 0x2020'0180);
    run({"vaddubm v1, v2, v3", "vaddubs v4, v2, v3", "vaddsbs v5, v2, v3"});
    CHECK(v(1).word(0) == 0x1010'8000);
    CHECK(v(4).word(0) == 0xffff'80ff); // unsigned: 0xf0 + 0x20 saturates to 0xff
    CHECK(v(5).word(0) == 0x1010'7f80); // signed: 0x7f + 1 → 0x7f, -128 + -128 → -128
    CHECK(saturated());
}

TEST_CASE_METHOD(VectorTest, "SAT is sticky") {
    state().vscr |= k_vscr_saturated;
    all_words(2, 1);
    run({"vadduws v1, v2, v2"});
    CHECK(saturated());
}

TEST_CASE_METHOD(VectorTest, "carries and saturating subtracts") {
    words(2, 0xffff'ffff, 5, 3, 0);
    words(3, 1, 5, 4, 1);
    run({"vaddcuw v1, v2, v3", "vsubcuw v4, v2, v3", "vsubuws v5, v2, v3"});
    CHECK(v(1) == Vector::from_words(1, 0, 0, 0));
    CHECK(v(4) == Vector::from_words(1, 1, 0, 0)); // 1 when a >= b
    CHECK(v(5) == Vector::from_words(0xffff'fffe, 0, 0, 0));
    CHECK(saturated());
}

TEST_CASE_METHOD(VectorTest, "max, min and average are signed or unsigned") {
    all_words(2, 0xffff'0001); // halves: -1 (or 65535), 1
    all_words(3, 0x0001'0002);
    run({"vmaxsh v1, v2, v3", "vmaxuh v4, v2, v3", "vminsh v5, v2, v3", "vavguh v6, v2, v3",
         "vavgsh v7, v2, v3"});
    CHECK(v(1).word(0) == 0x0001'0002);
    CHECK(v(4).word(0) == 0xffff'0002);
    CHECK(v(5).word(0) == 0xffff'0001);
    CHECK(v(6).word(0) == 0x8000'0002); // (65535 + 1 + 1) / 2, (1 + 2 + 1) / 2
    CHECK(v(7).word(0) == 0x0000'0002); // (-1 + 1 + 1) / 2 = 0
}

TEST_CASE_METHOD(VectorTest, "shifts and rotates use the element's low bits of b") {
    all_words(2, 0x8000'0001);
    words(3, 36, 1, 31, 32);
    run({"vrlw v1, v2, v3", "vslw v4, v2, v3", "vsraw v5, v2, v3", "vsrw v6, v2, v3"});
    CHECK(v(1) == Vector::from_words(0x0000'0018, 0x0000'0003, 0xc000'0000, 0x8000'0001));
    CHECK(v(4) == Vector::from_words(0x0000'0010, 0x0000'0002, 0x8000'0000, 0x8000'0001));
    CHECK(v(5).word(2) == 0xffff'ffff);
    CHECK(v(6).word(2) == 1);
}

TEST_CASE_METHOD(VectorTest, "logical operations") {
    all_words(2, 0xff00'ff00);
    all_words(3, 0x0ff0'0ff0);
    run({"vand v1, v2, v3", "vandc v4, v2, v3", "vor v5, v2, v3", "vnor v6, v2, v3",
         "vxor v7, v2, v3"});
    CHECK(v(1).word(0) == 0x0f00'0f00);
    CHECK(v(4).word(0) == 0xf000'f000);
    CHECK(v(5).word(0) == 0xfff0'fff0);
    CHECK(v(6).word(0) == 0x000f'000f);
    CHECK(v(7).word(0) == 0xf0f0'f0f0);
}

TEST_CASE_METHOD(VectorTest, "even and odd multiplies give double-width products") {
    all_words(2, 0xff02'fffe); // halves -254 (or 65282) and -2 (or 65534)
    all_words(3, 0x0003'0004);
    run({"vmuleuh v1, v2, v3", "vmulosh v4, v2, v3", "vmuloub v5, v2, v3"});
    CHECK(v(1).word(0) == 0xff02 * 3);
    CHECK(v(4).word(0) == static_cast<u32>(-2 * 4));
    CHECK(v(5).half(0) == 0x02 * 0x03); // odd bytes: 0x02 × 0x03
}

TEST_CASE_METHOD(VectorTest, "multiply-high-add saturates") {
    all_words(2, 0x4000'7fff);
    all_words(3, 0x4000'7fff);
    all_words(4, 0x0001'000a);
    run({"vmhraddshs v1, v2, v3, v4"});
    // (0x4000 × 0x4000 + 0x4000) >> 15 = 8192, + 1; (0x7fff² + 0x4000) >> 15 = 32767, + 10 →
    // clamped
    CHECK(v(1).word(0) == 0x2001'7fff);
    CHECK(saturated());
}

TEST_CASE_METHOD(VectorTest, "multiply-sums") {
    all_words(2, 0x0102'0304);
    all_words(3, 0x0506'0708);
    all_words(4, 100);
    run({"vmsumubm v1, v2, v3, v4", "vmladduhm v5, v2, v3, v4"});
    CHECK(v(1).word(0) == 1 * 5 + 2 * 6 + 3 * 7 + 4 * 8 + 100);
    // Halves: (0x0102 × 0x0506 + 0) and (0x0304 × 0x0708 + 100), each kept to 16 bits.
    const u32 high = (0x0102u * 0x0506u) & 0xffff;
    const u32 low = (0x0304u * 0x0708u + 100) & 0xffff;
    CHECK(v(5).word(0) == ((high << 16) | low));
}

TEST_CASE_METHOD(VectorTest, "signed multiply-sum of bytes and saturating halfword sums") {
    all_words(2, 0xff02'0000); // signed bytes -1, 2
    all_words(3, 0x0a0a'0000); // unsigned bytes 10, 10
    all_words(4, 0);
    words(5, 0x7fff'7fff, 0, 0, 0);
    words(6, 0x7fff'7fff, 0, 0, 0);
    words(7, 0x7fff'ffff, 0, 0, 0);
    run({"vmsummbm v1, v2, v3, v4", "vmsumshs v8, v5, v6, v7"});
    CHECK(v(1).word(0) == static_cast<u32>(-10 + 20));
    CHECK(v(8).word(0) == 0x7fff'ffff);
    CHECK(saturated());
}

TEST_CASE_METHOD(VectorTest, "sums across") {
    words(2, 0xffff'ffff, 1, 2, 3);
    words(3, 10, 20, 30, 40);
    run({"vsum4ubs v1, v2, v3", "vsumsws v4, v2, v3", "vsum2sws v5, v2, v3"});
    CHECK(v(1) == Vector::from_words(255 * 4 + 10, 21, 32, 43));
    CHECK(v(4) == Vector::from_words(0, 0, 0, static_cast<u32>(-1 + 1 + 2 + 3 + 40)));
    CHECK(v(5) == Vector::from_words(0, static_cast<u32>(-1 + 1 + 20), 0, 2 + 3 + 40));
}

// ---- Compare ----

TEST_CASE_METHOD(VectorTest, "compares give all-ones elements and summarize in CR6") {
    words(2, 1, 2, 3, 4);
    words(3, 1, 0, 3, 9);
    run({"vcmpequw. v1, v2, v3"});
    CHECK(v(1) == Vector::from_words(~0u, 0, ~0u, 0));
    CHECK(cr_field(6) == 0); // some true, some false

    run({"vcmpequw. v1, v2, v2"});
    CHECK(cr_field(6) == 0b1000); // all true

    run({"vcmpgtsw. v1, v3, v2", "vcmpgtub v4, v2, v3"});
    CHECK(cr_field(6) == 0); // 9 > 4 only
    CHECK(v(4) == Vector::from_words(0, 0x0000'00ff, 0, 0));
}

// ---- Loads and stores ----

TEST_CASE_METHOD(VectorTest, "lvx and stvx ignore the low address bits") {
    for (u32 i = 0; i < 32; ++i) {
        memory.write<u8>(k_data + i, static_cast<u8>(i));
    }
    r(4) = k_data + 16 + 7;
    run({"lvx v1, r0, r4", "stvx v1, r0, r4"});
    CHECK(v(1).byte(0) == 16);
    CHECK(v(1).byte(15) == 31);
}

TEST_CASE_METHOD(VectorTest, "element loads and stores") {
    memory.write<u32>(k_data + 8, 0xcafef00d);
    r(4) = k_data + 9; // inside the word at offset 8
    words(2, 1, 2, 0x1122'3344, 4);
    run({"lvewx v1, r0, r4", "stvewx v2, r0, r4", "stvebx v2, r0, r4"});
    CHECK(v(1).word(2) == 0xcafef00d);
    CHECK(memory.read<u32>(k_data + 8) == 0x1122'3344);
}

TEST_CASE_METHOD(VectorTest, "lvsl and lvsr make permute controls for the misalignment") {
    r(4) = k_data + 3;
    run({"lvsl v1, r0, r4", "lvsr v2, r0, r4"});
    CHECK(v(1).byte(0) == 3);
    CHECK(v(1).byte(15) == 18);
    CHECK(v(2).byte(0) == 13);
    CHECK(v(2).byte(15) == 28);
}

TEST_CASE_METHOD(VectorTest, "Cell load/store left and right access unaligned quadwords") {
    for (u32 i = 0; i < 48; ++i) {
        memory.write<u8>(k_data + i, static_cast<u8>(i));
    }
    r(4) = k_data + 5;
    r(5) = k_data + 21;
    r(6) = k_data + 32;
    run({"lvlx v1, r0, r4", "lvrx v2, r0, r5", "vor v3, v1, v2", "lvrx v4, r0, r6"});
    for (u32 i = 0; i < 16; ++i)
        CHECK(v(3).byte(i) == 5 + i); // the 16 bytes at k_data + 5
    CHECK(v(1).byte(11) == 0);        // past the quadword: zero
    CHECK(v(4) == Vector{});          // aligned right load: nothing

    all_words(7, 0xeeee'eeee);
    run({"stvlx v7, r0, r4", "stvrx v7, r0, r5"});
    CHECK(memory.read<u8>(k_data + 4) == 4);
    for (u32 i = 5; i < 21; ++i) {
        CHECK(memory.read<u8>(k_data + i) == 0xee);
    }
    CHECK(memory.read<u8>(k_data + 21) == 21);
}

// ---- Permute ----

TEST_CASE_METHOD(VectorTest, "vperm picks bytes from a then b") {
    words(2, 0x0001'0203, 0x0405'0607, 0x0809'0a0b, 0x0c0d'0e0f);
    words(3, 0x1011'1213, 0x1415'1617, 0x1819'1a1b, 0x1c1d'1e1f);
    words(4, 0x1f00'1001, 0x2f2f'0f0f, 0, 0x0102'0304); // only the low 5 bits count
    run({"vperm v1, v2, v3, v4"});
    CHECK(v(1).word(0) == 0x1f00'1001);
    CHECK(v(1).word(1) == 0x0f0f'0f0f); // 0x2f & 31 = 15: byte 15 of a
}

TEST_CASE_METHOD(VectorTest, "vsel, vsldoi, and whole-register shifts") {
    all_words(2, 0x1234'5678);
    all_words(3, 0xabcd'ef01);
    all_words(4, 0xffff'0000);
    run({"vsel v1, v2, v3, v4", "vsldoi v5, v2, v3, 14"});
    CHECK(v(1).word(0) == 0xabcd'5678);
    CHECK(v(5).word(0) == 0x5678'abcd);

    words(6, 0x8000'0000, 0, 0, 1);
    all_words(7, 0x0101'0101); // shift by 1 bit; or by 0 octets for vslo
    all_words(8, 0x1010'1010); // 0x10 >> 3 = 2 octets
    run({"vsl v9, v6, v7", "vsr v10, v6, v7", "vslo v11, v6, v8"});
    CHECK(v(9) == Vector::from_words(0, 0, 0, 2));
    CHECK(v(10) == Vector::from_words(0x4000'0000, 0, 0, 0));
    CHECK(v(11) == Vector::from_words(0, 0, 0, 0x0001'0000)); // the last byte moved up two
}

TEST_CASE_METHOD(VectorTest, "merge, splat and splat immediate") {
    words(2, 1, 2, 3, 4);
    words(3, 5, 6, 7, 8);
    run({"vmrghw v1, v2, v3", "vmrglw v4, v2, v3", "vspltw v5, v3, 2", "vspltisb v6, -3",
         "vspltish v7, 15"});
    CHECK(v(1) == Vector::from_words(1, 5, 2, 6));
    CHECK(v(4) == Vector::from_words(3, 7, 4, 8));
    CHECK(v(5) == Vector::from_words(7, 7, 7, 7));
    CHECK(v(6).word(0) == 0xfdfd'fdfd);
    CHECK(v(7).word(0) == 0x000f'000f);
}

TEST_CASE_METHOD(VectorTest, "pack and unpack") {
    words(2, 0x0001'ffff, 0x8000'7fff, 0x0100'0002, 0x00ff'0003); // halves
    words(3, 0, 0, 0, 0);
    run({"vpkuhum v1, v2, v3", "vpkshus v4, v2, v3", "vpkshss v5, v2, v3"});
    CHECK(v(1).word(0) == 0x01ff'00ff);
    CHECK(v(4).word(0) == 0x0100'00ff); // -1 → 0, -32768 → 0, 32767 → 255
    CHECK(v(5).word(0) == 0x01ff'807f); // 1, -1, -32768 → -128, 32767 → 127
    CHECK(saturated());

    words(6, 0x80ff'7f01, 0, 0, 0);
    run({"vupkhsb v7, v6"});
    CHECK(v(7).word(0) == 0xff80'ffff);
    CHECK(v(7).word(1) == 0x007f'0001);
}

TEST_CASE_METHOD(VectorTest, "pixel pack and unpack") {
    words(2, 0x01f8'f8f8, 0x0000'0000, 0, 0);
    words(3, 0, 0, 0, 0);
    run({"vpkpx v1, v2, v3", "vupkhpx v4, v1"});
    CHECK(v(1).half(0) == 0xffff); // alpha bit, then three 5-bit channels of 0x1f
    CHECK(v(4).word(0) == 0xff1f'1f1f);
}

TEST_CASE_METHOD(VectorTest, "mfvscr and mtvscr") {
    words(2, 0, 0, 0, 0xffff'ffff);
    run({"mtvscr v2", "mfvscr v1"});
    CHECK(state().vscr == (k_vscr_non_java | k_vscr_saturated));
    CHECK(v(1) == Vector::from_words(0, 0, 0, k_vscr_non_java | k_vscr_saturated));
}

// ---- Floating point: IEEE mode ----

TEST_CASE_METHOD(VectorTest, "IEEE mode rounds to nearest") {
    ieee_mode(true);
    floats(2, 1.0f, 1.5f, -2.0f, 0.0f);
    floats(3, std::ldexp(1.5f, -24), 2.25f, 0.5f, -0.0f);
    run({"vaddfp v1, v2, v3", "vsubfp v4, v2, v3"});
    CHECK(v(1).word(0) == 0x3f80'0001); // 1 + 0.75 ulp rounds up
    CHECK(f(1, 1) == 3.75f);
    CHECK(f(4, 2) == -2.5f);
}

TEST_CASE_METHOD(VectorTest, "IEEE mode NaNs: the first NaN made quiet, or the default NaN") {
    ieee_mode(true);
    words(2, 0x7f80'0001, 0x7f80'0000, 0x3f80'0000, 0);
    words(3, 0x7fc0'1234, 0x7f80'0000, 0x7fc0'1234, 0);
    run({"vaddfp v1, v2, v3", "vsubfp v4, v2, v3"});
    CHECK(v(1).word(0) == 0x7fc0'0001);
    CHECK(v(1).word(2) == 0x7fc0'1234);
    CHECK(v(4).word(1) == 0x7fc0'0000); // infinity - infinity
}

TEST_CASE_METHOD(VectorTest, "non-Java mode flushes denormals to zero") {
    words(2, 0x0000'0001, 0x0080'0000, 0, 0); // a denormal; the smallest normal
    floats(3, 0.0f, 0.0f, 0.0f, 0.0f);
    floats(4, 1.0f, 0.5f, 0.0f, 0.0f);

    ieee_mode(false);
    run({"vaddfp v1, v2, v3", "vmaddfp v5, v2, v4, v3"});
    CHECK(v(1).word(0) == 1);
    CHECK(v(5).word(1) == 0x0040'0000); // 2^-127: a denormal result

    ieee_mode(true);
    run({"vaddfp v1, v2, v3", "vmaddfp v5, v2, v4, v3"});
    CHECK(v(1).word(0) == 0);
    CHECK(v(5).word(1) == 0);
}

TEST_CASE_METHOD(VectorTest, "vmaddfp rounds once") {
    ieee_mode(true);
    // (1 + 2^-12)(1 - 2^-12) - 1 = -2^-24 exactly.
    floats(2, 1.0f + std::ldexp(1.0f, -12), 0, 0, 0);
    floats(4, 1.0f - std::ldexp(1.0f, -12), 0, 0, 0);
    floats(3, -1.0f, 0, 0, 0);
    run({"vmaddfp v1, v2, v4, v3", "vnmsubfp v5, v2, v4, v3"});
    CHECK(f(1, 0) == -std::ldexp(1.0f, -24));
    // -(a×c - b) = -(2 - 2^-24): halfway between 2 - 2^-23 and 2, so it rounds to even, 2.
    CHECK(f(5, 0) == -2.0f);
}

TEST_CASE_METHOD(VectorTest, "max and min order -0 below +0") {
    ieee_mode(true);
    floats(2, 0.0f, -1.0f, 0, 0);
    floats(3, -0.0f, 2.0f, 0, 0);
    run({"vmaxfp v1, v2, v3", "vminfp v4, v2, v3"});
    CHECK(v(1).word(0) == 0);
    CHECK(v(4).word(0) == 0x8000'0000);
    CHECK(f(1, 1) == 2.0f);
}

TEST_CASE_METHOD(VectorTest, "estimates are close to the true values") {
    ieee_mode(true);
    floats(2, 2.0f, 8.0f, 0.5f, 0.0f);
    run({"vrefp v1, v2", "vrsqrtefp v3, v2", "vlogefp v4, v2", "vexptefp v5, v2"});
    CHECK(f(1, 0) == 0.5f);
    CHECK(std::fabs(f(3, 1) - 1.0f / std::sqrt(8.0f)) < 1e-6f);
    CHECK(f(4, 1) == 3.0f);
    CHECK(std::fabs(f(4, 2) - -1.0f) < 1e-6f);
    CHECK(std::fabs(f(5, 2) - std::sqrt(2.0f)) < 1e-5f);
    CHECK(f(5, 1) == 256.0f);
    CHECK(std::isinf(f(1, 3)));  // 1 / 0
    CHECK(f(4, 3) == -INFINITY); // log2(0)
}

TEST_CASE_METHOD(VectorTest, "rounding to integer in each direction") {
    floats(2, 2.5f, -2.7f, 2.1f, -2.1f);
    run({"vrfin v1, v2", "vrfiz v3, v2", "vrfip v4, v2", "vrfim v5, v2"});
    CHECK(f(1, 0) == 2.0f); // ties to even
    CHECK(f(3, 1) == -2.0f);
    CHECK(f(4, 2) == 3.0f);
    CHECK(f(5, 3) == -3.0f);
}

TEST_CASE_METHOD(VectorTest, "conversions between integers and floats, with scaling") {
    words(2, static_cast<u32>(-3), 0xffff'ffff, 7, 0);
    run({"vcfsx v1, v2, 1", "vcfux v3, v2, 0"});
    CHECK(f(1, 0) == -1.5f);
    CHECK(f(3, 1) == 4294967296.0f); // rounded to nearest

    floats(4, 1.5f, 3e9f, -1.0f, 0.75f);
    run({"vctsxs v5, v4, 1", "vctuxs v6, v4, 0"});
    CHECK(v(5) == Vector::from_words(3, 0x7fff'ffff, static_cast<u32>(-2), 1));
    CHECK(v(6) == Vector::from_words(1, 3'000'000'000u, 0, 0));
    CHECK(saturated());
}

TEST_CASE_METHOD(VectorTest, "float compares, and bounds") {
    ieee_mode(true);
    floats(2, 1.0f, 2.0f, -3.0f, NAN);
    floats(3, 1.0f, 1.0f, 2.0f, 1.0f);
    run({"vcmpeqfp v1, v2, v3", "vcmpgtfp v4, v2, v3", "vcmpbfp. v5, v2, v3"});
    CHECK(v(1) == Vector::from_words(~0u, 0, 0, 0));
    CHECK(v(4) == Vector::from_words(0, ~0u, 0, 0));
    CHECK(v(5) == Vector::from_words(0, 0x8000'0000, 0x4000'0000, 0xc000'0000));
    CHECK(cr_field(6) == 0); // not all in bounds
}

// ---- Floating point: graphics rounding mode (the Cell's default) ----

TEST_CASE_METHOD(VectorTest, "graphics mode rounds toward zero") {
    floats(2, 1.0f, 0, 0, 0);
    floats(3, std::ldexp(1.5f, -24), 0, 0, 0);
    run({"vaddfp v1, v2, v3"});
    CHECK(v(1).word(0) == 0x3f80'0000);
}

TEST_CASE_METHOD(VectorTest, "graphics mode has no infinities or NaNs") {
    // 0x7f800000 is 2^128 here, an ordinary number. Doubling it overflows to the largest value.
    words(2, 0x7f80'0000, 0x7f80'0000, 0x7fc0'0000, 0xff80'0000);
    words(3, 0x7f80'0000, 0x0000'0000, 0x3f80'0000, 0x7f80'0000);
    floats(4, 0.0f, 0.0f, 0.0f, 0.0f);
    run({"vaddfp v1, v2, v3", "vmaddfp v5, v2, v4, v3"});
    CHECK(v(1).word(0) == 0x7fff'ffff);
    CHECK(v(1).word(3) == 0x0000'0000); // -2^128 + 2^128
    CHECK(v(5).word(1) == 0);           // "infinity" × 0 is 0
    CHECK(v(5).word(2) == 0x3f80'0000); // "NaN" × 0 + 1 = 1
}

TEST_CASE_METHOD(VectorTest, "graphics mode log2 of zero is 0xffffffff") {
    floats(2, 0.0f, -8.0f, 0, 0);
    run({"vlogefp v1, v2"});
    CHECK(v(1).word(0) == 0xffff'ffff); // Programming Handbook, A.3.3.5
    CHECK(f(1, 1) == 3.0f);             // log2 of the absolute value
}
