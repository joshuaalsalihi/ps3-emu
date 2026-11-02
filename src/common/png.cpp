#include "common/png.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace {

// CRC-32 with the reflected polynomial 0xedb88320, as PNG chunks use. PNG specification, annex D.
u32 crc32(std::span<const u8> data) {
    u32 crc = 0xffff'ffff;
    for (const u8 byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb8'8320 : 0);
        }
    }
    return ~crc;
}

// RFC 1950, section 8.2.
u32 adler32(std::span<const u8> data) {
    u32 a = 1;
    u32 b = 0;
    for (const u8 byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void append_be32(std::vector<u8>& out, u32 value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<u8>(value >> shift));
    }
}

void append_chunk(std::vector<u8>& out, std::string_view type, std::span<const u8> data) {
    append_be32(out, static_cast<u32>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    append_be32(out, crc32(std::span(out).subspan(start)));
}

// A zlib stream holding `data` in stored (uncompressed) deflate blocks.
std::vector<u8> zlib_stored(std::span<const u8> data) {
    constexpr std::size_t k_max_block = 65535;
    std::vector<u8> out = {0x78, 0x01}; // deflate, 32 KB window, no dictionary
    std::size_t position = 0;
    do {
        const std::size_t size = std::min(k_max_block, data.size() - position);
        const bool last = position + size == data.size();
        out.push_back(last ? 1 : 0); // BFINAL, BTYPE = 00 (stored)
        out.push_back(static_cast<u8>(size));
        out.push_back(static_cast<u8>(size >> 8));
        out.push_back(static_cast<u8>(~size));
        out.push_back(static_cast<u8>(~size >> 8));
        out.insert(out.end(), data.begin() + position, data.begin() + position + size);
        position += size;
    } while (position < data.size());
    append_be32(out, adler32(data));
    return out;
}

} // namespace

std::vector<u8> encode_png(u32 width, u32 height, std::span<const u8> rgb) {
    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    std::vector<u8> header;
    append_be32(header, width);
    append_be32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0}); // 8 bits, RGB, deflate, filter 0, no interlace
    append_chunk(out, "IHDR", header);

    // Each row starts with its filter type: 0, none.
    std::vector<u8> rows;
    rows.reserve(std::size_t{height} * (1 + std::size_t{width} * 3));
    for (u32 y = 0; y < height; ++y) {
        rows.push_back(0);
        const auto row = rgb.subspan(std::size_t{y} * width * 3, std::size_t{width} * 3);
        rows.insert(rows.end(), row.begin(), row.end());
    }
    append_chunk(out, "IDAT", zlib_stored(rows));
    append_chunk(out, "IEND", {});
    return out;
}
