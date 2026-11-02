#include "common/png.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("a PNG holds the image uncompressed, with correct checksums") {
    // A 2x1 image: red, then blue. Expected bytes: Python's zlib.crc32 and zlib.adler32 over the
    // same chunks, and zlib.decompress of the IDAT data gives the two rows back.
    const std::vector<u8> rgb = {0xff, 0, 0, 0, 0, 0xff};
    const std::vector<u8> expected = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, // signature
        0x00, 0x00, 0x00, 0x0d, 'I',  'H',  'D',  'R',  // IHDR, 13 bytes
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, // 2 x 1
        0x08, 0x02, 0x00, 0x00, 0x00,                   // 8-bit RGB
        0x7b, 0x40, 0xe8, 0xdd,                         // CRC
        0x00, 0x00, 0x00, 0x12, 'I',  'D',  'A',  'T',  // IDAT, 18 bytes
        0x78, 0x01,                                     // zlib header
        0x01, 0x07, 0x00, 0xf8, 0xff,                   // final stored block, 7 bytes
        0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff,       // filter 0, then the pixels
        0x07, 0x00, 0x01, 0xff,                         // Adler-32
        0x55, 0x36, 0xba, 0xc7,                         // CRC
        0x00, 0x00, 0x00, 0x00, 'I',  'E',  'N',  'D',  0xae, 0x42, 0x60, 0x82,
    };
    CHECK(encode_png(2, 1, rgb) == expected);
}
