#pragma once

#include "common/types.h"

#include <span>
#include <vector>

// Encodes an 8-bit RGB image (top row first) as a PNG file. The image data is stored without
// compression, which keeps the encoder short. PNG specification (W3C, 3rd edition); zlib and
// deflate stored blocks: RFC 1950 and RFC 1951.
std::vector<u8> encode_png(u32 width, u32 height, std::span<const u8> rgb);
