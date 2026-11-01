#include "support/spu_test.h"

#include <catch2/catch_test_macros.hpp>

using test::SpuTest;

TEST_CASE_METHOD(SpuTest, "halfword shifts and rotates use each halfword's own count") {
    set_all(1, 0x8001'00ff);
    set_words(2, 0x0001'000f, 0x0010'0011, 0x001f'0020, 0x0000'0008);
    run({"shlh $3, $1, $2", "shlhi $4, $1, 3", "roth $9, $1, $2", "rothi $10, $1, -1"});
    // Shift counts of 16 or more give zero; only the low five bits of the count are used.
    CHECK(r(3) == Vector::from_words(0x0002'8000, 0, 0x0000'00ff, 0x8001'ff00));
    CHECK(r(4).word(0) == 0x0008'07f8);
    // Rotates use the low four bits.
    CHECK(r(9).word(0) == 0x0003'807f);
    CHECK(r(9).word(1) == 0x8001'01fe);
    CHECK(r(10).word(0) == 0xc000'807f);
}

TEST_CASE_METHOD(SpuTest, "word shifts and rotates") {
    set_all(5, 0x8000'0001);
    set_words(6, 1, 31, 32, 63);
    run({"shl $7, $5, $6", "shli $8, $5, 4", "rot $11, $5, $6", "roti $12, $5, -4"});
    CHECK(r(7) == Vector::from_words(0x0000'0002, 0x8000'0000, 0, 0));
    CHECK(r(8).word(0) == 0x0000'0010);
    CHECK(r(11) == Vector::from_words(0x0000'0003, 0xc000'0000, 0x8000'0001, 0xc000'0000));
    CHECK(r(12).word(0) == 0x1800'0000);
}

TEST_CASE_METHOD(SpuTest, "rotate and mask shifts right by the negated count") {
    set_all(1, 0x8001'00ff);
    set_words(13, 0xffff'fff1, 0xfff0'0000, 0, 0); // halfword counts -1, -15, -16, 0
    set_all(5, 0x8000'0001);
    set_words(16, static_cast<u32>(-1), static_cast<u32>(-31), static_cast<u32>(-32), 0);
    run({"rothm $14, $1, $13", "rothmi $15, $1, -8", "rotm $17, $5, $16", "rotmi $18, $5, -4"});
    CHECK(r(14).word(0) == 0x4000'0000);
    CHECK(r(14).word(1) == 0x0000'00ff); // a shift of 16 gives zero
    CHECK(r(15).word(0) == 0x0080'0000);
    CHECK(r(17) == Vector::from_words(0x4000'0000, 1, 0, 0x8000'0001));
    CHECK(r(18).word(0) == 0x0800'0000);
}

TEST_CASE_METHOD(SpuTest, "algebraic rotate and mask fills with the sign bit") {
    set_all(1, 0x8001'00ff);
    set_words(13, 0xffff'fff1, 0xfff0'0000, 0, 0);
    set_all(5, 0x8000'0001);
    set_words(16, static_cast<u32>(-1), static_cast<u32>(-31), static_cast<u32>(-32), 0);
    run({"rotmah $18, $1, $13", "rotmahi $19, $1, -4", "rotma $20, $5, $16",
         "rotmai $21, $5, -31"});
    CHECK(r(18).word(0) == 0xc000'0000);
    CHECK(r(18).word(1) == 0xffff'00ff); // a shift of 16 copies the sign everywhere
    CHECK(r(19).word(0) == 0xf800'000f);
    CHECK(r(20) == Vector::from_words(0xc000'0000, 0xffff'ffff, 0xffff'ffff, 0x8000'0001));
    CHECK(r(21).word(0) == 0xffff'ffff);
}

namespace {

const Vector k_quad = Vector::from_words(0x0123'4567, 0x89ab'cdef, 0xfedc'ba98, 0x7654'3210);
const Vector k_quad_shifted_left_3_bytes =
    Vector::from_words(0x6789'abcd, 0xeffe'dcba, 0x9876'5432, 0x1000'0000);
const Vector k_quad_rotated_left_3_bytes =
    Vector::from_words(0x6789'abcd, 0xeffe'dcba, 0x9876'5432, 0x1001'2345);
const Vector k_quad_shifted_right_3_bytes =
    Vector::from_words(0x0000'0001, 0x2345'6789, 0xabcd'effe, 0xdcba'9876);

} // namespace

TEST_CASE_METHOD(SpuTest, "quadword shifts left by bits and bytes") {
    r(1) = k_quad;
    set_all(2, 0xc);  // shlqbi uses the low three bits: 4
    set_all(3, 0x23); // shlqby uses the low five bits: 3
    set_all(4, 0x1f); // shlqbybi uses bits 24-28, the bit count / 8: 3
    set_all(5, 16);
    run({"shlqbi $10, $1, $2", "shlqbii $11, $1, 4", "shlqby $12, $1, $3", "shlqbyi $13, $1, 3",
         "shlqbybi $14, $1, $4", "shlqby $15, $1, $5"});
    const Vector shifted_4_bits =
        Vector::from_words(0x1234'5678, 0x9abc'deff, 0xedcb'a987, 0x6543'2100);
    CHECK(r(10) == shifted_4_bits);
    CHECK(r(11) == shifted_4_bits);
    CHECK(r(12) == k_quad_shifted_left_3_bytes);
    CHECK(r(13) == k_quad_shifted_left_3_bytes);
    CHECK(r(14) == k_quad_shifted_left_3_bytes);
    CHECK(r(15) == Vector{}); // 16 bytes or more shifts everything out
}

TEST_CASE_METHOD(SpuTest, "quadword rotates by bits and bytes") {
    r(1) = k_quad;
    set_all(2, 19); // rotqby uses the low four bits: 3
    set_all(3, 24); // 24 bits = 3 bytes
    set_words(6, 0x8000'0000, 0, 0, 1);
    set_all(7, 4);
    run({"rotqby $10, $1, $2", "rotqbyi $11, $1, 3", "rotqbybi $12, $1, $3", "rotqbi $13, $6, $7",
         "rotqbii $14, $6, 4"});
    CHECK(r(10) == k_quad_rotated_left_3_bytes);
    CHECK(r(11) == k_quad_rotated_left_3_bytes);
    CHECK(r(12) == k_quad_rotated_left_3_bytes);
    CHECK(r(13) == Vector::from_words(0, 0, 0, 0x18));
    CHECK(r(14) == Vector::from_words(0, 0, 0, 0x18));
}

TEST_CASE_METHOD(SpuTest, "quadword rotate and mask shifts right by the negated count") {
    r(1) = k_quad;
    set_all(2, static_cast<u32>(-3));
    set_all(3, static_cast<u32>(-24)); // -24 bits: three bytes
    set_all(4, static_cast<u32>(-4));
    set_all(5, static_cast<u32>(-16));
    run({"rotqmby $10, $1, $2", "rotqmbyi $11, $1, -3", "rotqmbybi $12, $1, $3",
         "rotqmbi $13, $1, $4", "rotqmbii $14, $1, -4", "rotqmby $15, $1, $5"});
    const Vector shifted_4_bits =
        Vector::from_words(0x0012'3456, 0x789a'bcde, 0xffed'cba9, 0x8765'4321);
    CHECK(r(10) == k_quad_shifted_right_3_bytes);
    CHECK(r(11) == k_quad_shifted_right_3_bytes);
    CHECK(r(12) == k_quad_shifted_right_3_bytes);
    CHECK(r(13) == shifted_4_bits);
    CHECK(r(14) == shifted_4_bits);
    CHECK(r(15) == Vector{});
}
