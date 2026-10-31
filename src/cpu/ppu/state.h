#pragma once

#include "common/types.h"
#include "common/vector.h"

#include <array>

namespace ppu {

// Fixed-point exception register.
struct Xer {
    bool so = false; // summary overflow: sticky copy of OV
    bool ov = false; // overflow
    bool ca = false; // carry
    u8 byte_count = 0;

    u64 value() const { return (u64{so} << 31) | (u64{ov} << 30) | (u64{ca} << 29) | byte_count; }
    void set(u64 value) {
        so = (value >> 31) & 1;
        ov = (value >> 30) & 1;
        ca = (value >> 29) & 1;
        byte_count = value & 0x7f;
    }
};

// Condition register values for a 4-bit field (LT, GT, EQ, SO).
constexpr u32 k_cr_lt = 0b1000;
constexpr u32 k_cr_gt = 0b0100;
constexpr u32 k_cr_eq = 0b0010;
constexpr u32 k_cr_so = 0b0001;

// Special-purpose register numbers (mfspr/mtspr).
constexpr u32 k_spr_xer = 1;
constexpr u32 k_spr_lr = 8;
constexpr u32 k_spr_ctr = 9;
constexpr u32 k_spr_vrsave = 256;
constexpr u32 k_spr_tb = 268;
constexpr u32 k_spr_tbu = 269;

// Vector status and control register bits.
constexpr u32 k_vscr_non_java = 1u << 16; // NJ: denormals are treated as zero
constexpr u32 k_vscr_saturated = 1;       // SAT: a saturating instruction clamped (sticky)

// User-visible PPU registers.
struct State {
    std::array<u64, 32> gpr{};
    std::array<u64, 32> fpr{}; // IEEE double bit patterns
    std::array<Vector, 32> vr{};
    u32 fpscr = 0;
    // UNVERIFIED: the VSCR value LV2 starts threads with. Non-Java mode is the usual default.
    u32 vscr = k_vscr_non_java;
    u32 cr = 0;
    u64 lr = 0;
    u64 ctr = 0;
    Xer xer;
    u32 vrsave = 0;
    u64 pc = 0;

    // Condition register bits and fields, numbered from the most significant bit.
    bool cr_bit(u32 bit) const { return (cr >> (31 - bit)) & 1; }
    void set_cr_bit(u32 bit, bool value) {
        const u32 mask = u32{1} << (31 - bit);
        cr = value ? (cr | mask) : (cr & ~mask);
    }
    u32 cr_field(u32 field) const { return (cr >> (28 - 4 * field)) & 0xf; }
    void set_cr_field(u32 field, u32 value) {
        const u32 shift = 28 - 4 * field;
        cr = (cr & ~(u32{0xf} << shift)) | ((value & 0xf) << shift);
    }
};

} // namespace ppu
