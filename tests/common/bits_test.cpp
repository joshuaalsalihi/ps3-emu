#include "common/bits.h"
#include "common/endian.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("sign_extend") {
    CHECK(sign_extend(0x7f, 8) == 127);
    CHECK(sign_extend(0x80, 8) == -128);
    CHECK(sign_extend(0x1ff80, 8) == -128); // bits above the field are ignored
    CHECK(sign_extend(0xffff'ffff, 32) == -1);
}

TEST_CASE("rotl32 rotates a duplicated low word") {
    CHECK(rotl32(0xffff'ffff'0000'0001, 0) == 0x0000'0001'0000'0001);
    CHECK(rotl32(0x8000'0000, 1) == 0x0000'0001'0000'0001);
}

TEST_CASE("mask64 uses PowerPC bit numbering and wraps when mb > me") {
    CHECK(mask64(0, 63) == ~u64{0});
    CHECK(mask64(0, 0) == 0x8000'0000'0000'0000);
    CHECK(mask64(63, 63) == 1);
    CHECK(mask64(32, 63) == 0xffff'ffff);
    CHECK(mask64(60, 3) == 0xf000'0000'0000'000f);
}

TEST_CASE("128-bit multiply high halves") {
    // Expected values cross-checked with Python's arbitrary-precision integers.
    CHECK(mul_high_unsigned(~u64{0}, ~u64{0}) == 0xffff'ffff'ffff'fffe);
    CHECK(mul_high_unsigned(0x0123'4567'89ab'cdef, 0xfedc'ba98'7654'3210) == 0x0121'fa00'ad77'd742);
    CHECK(mul_high_signed(-1, -1) == 0);
    CHECK(mul_high_signed(-1, 1) == -1);
    CHECK(mul_high_signed(0x0123'4567'89ab'cdef, static_cast<s64>(0xfedc'ba98'7654'3210)) ==
          static_cast<s64>(0xfffe'b499'23cc'0953));
}

TEST_CASE("byte_swap and store_be") {
    CHECK(byte_swap<u16>(0x1122) == 0x2211);
    CHECK(byte_swap<u32>(0x11223344) == 0x44332211);
    CHECK(byte_swap<u64>(0x1122334455667788) == 0x8877665544332211);

    u8 bytes[4] = {};
    store_be<u32>(bytes, 0x01020304);
    CHECK(bytes[0] == 0x01);
    CHECK(bytes[3] == 0x04);
}
