#pragma once

// The Floating-Point Status and Control Register: bit names and the rules for updating it.
// Reference: Power ISA v3.1, Book I, section 4.2.2. Bits are named as in the manual; bit 0 (FX)
// is the most significant bit of the 32-bit register.

#include "common/soft_float.h"
#include "common/types.h"

namespace ppu::fpscr {

// Exception bits. All of these are sticky.
constexpr u32 k_fx = 1u << 31;     // some exception bit went from 0 to 1
constexpr u32 k_fex = 1u << 30;    // summary: an enabled exception is set
constexpr u32 k_vx = 1u << 29;     // summary: some invalid-operation bit is set
constexpr u32 k_ox = 1u << 28;     // overflow
constexpr u32 k_ux = 1u << 27;     // underflow
constexpr u32 k_zx = 1u << 26;     // zero divide
constexpr u32 k_xx = 1u << 25;     // inexact
constexpr u32 k_vxsnan = 1u << 24; // invalid: signaling NaN operand
constexpr u32 k_vxisi = 1u << 23;  // invalid: infinity - infinity
constexpr u32 k_vxidi = 1u << 22;  // invalid: infinity / infinity
constexpr u32 k_vxzdz = 1u << 21;  // invalid: 0 / 0
constexpr u32 k_vximz = 1u << 20;  // invalid: infinity × 0
constexpr u32 k_vxvc = 1u << 19;   // invalid: ordered compare with a NaN
constexpr u32 k_vxsoft = 1u << 10; // invalid: software-defined
constexpr u32 k_vxsqrt = 1u << 9;  // invalid: square root of a negative number
constexpr u32 k_vxcvi = 1u << 8;   // invalid: integer conversion out of range

// Status of the last rounding (not sticky).
constexpr u32 k_fr = 1u << 18; // fraction rounded: the significand was incremented
constexpr u32 k_fi = 1u << 17; // fraction inexact

// Result flags (FPRF): a class bit, then the condition code (FPCC).
constexpr u32 k_fprf_mask = 0x1f << 12;
constexpr u32 k_fpcc_mask = 0xf << 12;
constexpr u32 k_c = 1u << 16;
constexpr u32 k_fl = 1u << 15; // less than, or negative
constexpr u32 k_fg = 1u << 14; // greater than, or positive
constexpr u32 k_fe = 1u << 13; // equal, or zero
constexpr u32 k_fu = 1u << 12; // unordered, or NaN

// Enables and rounding control.
constexpr u32 k_ve = 1u << 7;
constexpr u32 k_oe = 1u << 6;
constexpr u32 k_ue = 1u << 5;
constexpr u32 k_ze = 1u << 4;
constexpr u32 k_xe = 1u << 3;
constexpr u32 k_ni = 1u << 2;
constexpr u32 k_rn_mask = 3;

constexpr u32 k_invalid_bits =
    k_vxsnan | k_vxisi | k_vxidi | k_vxzdz | k_vximz | k_vxvc | k_vxsoft | k_vxsqrt | k_vxcvi;
constexpr u32 k_exception_bits = k_ox | k_ux | k_zx | k_xx | k_invalid_bits;

// Recomputes the summary bits VX and FEX, which are never set directly.
constexpr u32 with_summaries(u32 fpscr) {
    fpscr &= ~(k_vx | k_fex);
    if (fpscr & k_invalid_bits) fpscr |= k_vx;
    const bool enabled = ((fpscr & k_vx) && (fpscr & k_ve)) || ((fpscr & k_ox) && (fpscr & k_oe)) ||
                         ((fpscr & k_ux) && (fpscr & k_ue)) || ((fpscr & k_zx) && (fpscr & k_ze)) ||
                         ((fpscr & k_xx) && (fpscr & k_xe));
    if (enabled) fpscr |= k_fex;
    return fpscr;
}

// Sets exception bits, and FX if any of them is newly set.
constexpr u32 with_exceptions(u32 fpscr, u32 exceptions) {
    if ((exceptions & k_exception_bits & ~fpscr) != 0) fpscr |= k_fx;
    return with_summaries(fpscr | exceptions);
}

// The rounding mode and exception enables, for soft_float.
constexpr soft_float::Environment environment(u32 fpscr) {
    constexpr soft_float::Rounding k_modes[] = {
        soft_float::Rounding::nearest_even, soft_float::Rounding::toward_zero,
        soft_float::Rounding::toward_positive, soft_float::Rounding::toward_negative};
    return {k_modes[fpscr & k_rn_mask], (fpscr & k_oe) != 0, (fpscr & k_ue) != 0};
}

// The FPRF value describing a result. Single-precision results are classified by single
// precision's ranges: a value too small to be a single-precision normal is "denormalized",
// even though it is a normal double.
u32 result_class(u64 bits, soft_float::Format format);

} // namespace ppu::fpscr
