#include "common/sha1.h"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>

namespace {

std::string sha1_hex(std::string_view text) {
    const auto digest = sha1({reinterpret_cast<const u8*>(text.data()), text.size()});
    std::string hex;
    for (const u8 byte : digest)
        hex += std::format("{:02x}", byte);
    return hex;
}

} // namespace

TEST_CASE("SHA-1 matches the FIPS 180 test vectors") {
    CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // Two blocks after padding.
    CHECK(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}
