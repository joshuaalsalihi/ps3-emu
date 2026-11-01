#pragma once

#include "common/types.h"
#include "common/vector.h"

#include <array>

namespace spu {

// The local store size, and the mask (LSLR, "local storage limit register") applied to every
// local store address, so addresses wrap around. SPU ISA v1.2, section 2.
constexpr u32 k_local_store_size = 256 * 1024;
constexpr u32 k_lslr = k_local_store_size - 1;

// FPSCR bits, numbered as in the SPU ISA (v1.2, section 9.3) from the most significant bit of
// the 128-bit register. Single-precision flags (OVF, UNF, DIFF) are three bits per word slot;
// double-precision flags (OVF, UNF, INX, INV, NAN, DENORM) are six bits per doubleword slot.
constexpr u32 k_fpscr_rounding_slot0 = 20; // RN0, two bits
constexpr u32 k_fpscr_rounding_slot1 = 22; // RN1, two bits
constexpr u32 k_fpscr_single_flags[4] = {29, 61, 93, 125};
constexpr u32 k_fpscr_double_flags[2] = {50, 82};
constexpr u32 k_fpscr_divide_by_zero = 116; // one bit per word slot, 116-119

// Single-precision flags, as offsets from a slot's first flag bit.
constexpr u32 k_single_overflow = 0;
constexpr u32 k_single_underflow = 1;
constexpr u32 k_single_diff = 2; // the result may differ from IEEE's

// Double-precision flags, as offsets from a slot's first flag bit.
constexpr u32 k_double_overflow = 0;
constexpr u32 k_double_underflow = 1;
constexpr u32 k_double_inexact = 2;
constexpr u32 k_double_invalid = 3;
constexpr u32 k_double_nan = 4;    // a NaN result didn't propagate the input NaN
constexpr u32 k_double_denorm = 5; // a denormal operand was treated as zero

// User-visible SPU registers.
struct State {
    // 128 general-purpose registers. Scalar values live in the "preferred slot": word 0 for
    // words and addresses, bytes 2-3 for halfwords, byte 3 for bytes, doubleword 0.
    std::array<Vector, 128> gpr{};
    Vector fpscr;
    u32 pc = 0;
    u32 srr0 = 0; // where iret returns to
    bool interrupts_enabled = false;

    bool fpscr_bit(u32 bit) const { return (fpscr.byte(bit / 8) >> (7 - bit % 8)) & 1; }
    void set_fpscr_bit(u32 bit) {
        fpscr.set_byte(bit / 8, static_cast<u8>(fpscr.byte(bit / 8) | (0x80 >> (bit % 8))));
    }
};

} // namespace spu
