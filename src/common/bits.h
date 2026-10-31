#pragma once

// Bit manipulation helpers. Bit numbers follow the PowerPC manuals: bit 0 is the most
// significant bit of a 64-bit value.

#include "common/types.h"

#include <bit>

// Sign-extends the low `bit_count` bits of `value`.
constexpr s64 sign_extend(u64 value, u32 bit_count) {
    const u64 sign_bit = u64{1} << (bit_count - 1);
    const u64 low_bits = value & ((sign_bit << 1) - 1);
    return static_cast<s64>((low_bits ^ sign_bit) - sign_bit);
}

constexpr u64 rotl64(u64 value, u32 amount) {
    return std::rotl(value, static_cast<int>(amount % 64));
}

// ROTL32 from the PowerPC manuals: rotates the low word as if it were duplicated in both
// halves of a 64-bit register.
constexpr u64 rotl32(u64 value, u32 amount) {
    const u64 word = value & 0xffff'ffff;
    return rotl64(word | (word << 32), amount);
}

// MASK(mb, me) from the PowerPC manuals: ones from bit `mb` to bit `me` inclusive. If mb > me
// the mask wraps around, leaving zeros from me + 1 to mb - 1.
constexpr u64 mask64(u32 mb, u32 me) {
    const u64 from_mb = ~u64{0} >> mb;
    const u64 to_me = ~u64{0} << (63 - me);
    return mb <= me ? (from_mb & to_me) : (from_mb | to_me);
}

// High 64 bits of the 128-bit product a * b, computed from 32-bit halves.
constexpr u64 mul_high_unsigned(u64 a, u64 b) {
    const u64 a_lo = a & 0xffff'ffff, a_hi = a >> 32;
    const u64 b_lo = b & 0xffff'ffff, b_hi = b >> 32;

    const u64 lo_lo = a_lo * b_lo;
    const u64 hi_lo = a_hi * b_lo;
    const u64 lo_hi = a_lo * b_hi;
    const u64 hi_hi = a_hi * b_hi;

    const u64 middle = (lo_lo >> 32) + (hi_lo & 0xffff'ffff) + (lo_hi & 0xffff'ffff);
    return hi_hi + (hi_lo >> 32) + (lo_hi >> 32) + (middle >> 32);
}

// High 64 bits of the signed 128-bit product a * b.
constexpr s64 mul_high_signed(s64 a, s64 b) {
    // Treating a negative operand as unsigned adds 2^64 times the other operand to the
    // product; subtract that back out of the high half.
    u64 high = mul_high_unsigned(static_cast<u64>(a), static_cast<u64>(b));
    if (a < 0) high -= static_cast<u64>(b);
    if (b < 0) high -= static_cast<u64>(a);
    return static_cast<s64>(high);
}
