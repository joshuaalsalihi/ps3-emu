#pragma once

#include "common/endian.h"
#include "common/types.h"

#include <array>

namespace ppu {

// A 128-bit vector register (VMX). Elements are numbered from the most significant end, as in
// the manuals, so element 0 is the one at the lowest memory address. The bytes are stored in
// memory order, so loads and stores copy them unchanged.
struct Vector {
    std::array<u8, 16> bytes{};

    u8 byte(u32 i) const { return bytes[i]; }
    u16 half(u32 i) const { return load_be<u16>(&bytes[2 * i]); }
    u32 word(u32 i) const { return load_be<u32>(&bytes[4 * i]); }

    void set_byte(u32 i, u8 value) { bytes[i] = value; }
    void set_half(u32 i, u16 value) { store_be(&bytes[2 * i], value); }
    void set_word(u32 i, u32 value) { store_be(&bytes[4 * i], value); }

    // The whole vector as two 64-bit halves, most significant first.
    u64 high() const { return load_be<u64>(&bytes[0]); }
    u64 low() const { return load_be<u64>(&bytes[8]); }

    static Vector from_words(u32 w0, u32 w1, u32 w2, u32 w3) {
        Vector v;
        v.set_word(0, w0);
        v.set_word(1, w1);
        v.set_word(2, w2);
        v.set_word(3, w3);
        return v;
    }

    friend bool operator==(const Vector&, const Vector&) = default;
};

} // namespace ppu
