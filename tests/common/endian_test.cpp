#include "common/endian.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("load_be reads the most significant byte first") {
    const u8 bytes[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

    CHECK(load_be<u8>(bytes) == 0x01);
    CHECK(load_be<u16>(bytes) == 0x0123);
    CHECK(load_be<u32>(bytes) == 0x01234567);
    CHECK(load_be<u64>(bytes) == 0x0123456789abcdef);
}
