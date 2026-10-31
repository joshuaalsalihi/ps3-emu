#pragma once

// Helpers shared by the vector (VMX) instruction files: element access by width, saturation,
// and the CR6 summary that vector compares record.

#include "common/bits.h"
#include "cpu/ppu/opcodes.h"

#include <algorithm>

namespace ppu::vmx {

// Element widths. The value is the size in bytes.
enum class Width : u32 { byte = 1, half = 2, word = 4 };

constexpr u32 count(Width width) {
    return 16 / static_cast<u32>(width);
}

constexpr u32 bit_size(Width width) {
    return 8 * static_cast<u32>(width);
}

inline u32 get(const Vector& v, Width width, u32 i) {
    switch (width) {
    case Width::byte: return v.byte(i);
    case Width::half: return v.half(i);
    case Width::word: return v.word(i);
    }
    return 0;
}

// Stores the low `width` bits of `value` (results wrap, as "modulo" instructions do).
inline void set(Vector& v, Width width, u32 i, u32 value) {
    switch (width) {
    case Width::byte: v.set_byte(i, static_cast<u8>(value)); break;
    case Width::half: v.set_half(i, static_cast<u16>(value)); break;
    case Width::word: v.set_word(i, value); break;
    }
}

// An element read as a signed number.
constexpr s64 as_signed(u32 value, Width width) {
    return sign_extend(value, bit_size(width));
}

// Clamps to the signed or unsigned range of `width`, noting whether it had to.
constexpr u32 saturate_signed(s64 value, Width width, bool& saturated) {
    const s64 max = (s64{1} << (bit_size(width) - 1)) - 1;
    const s64 min = -max - 1;
    if (value > max || value < min) saturated = true;
    return static_cast<u32>(std::clamp(value, min, max));
}

constexpr u32 saturate_unsigned(s64 value, Width width, bool& saturated) {
    const s64 max = (s64{1} << bit_size(width)) - 1;
    if (value > max || value < 0) saturated = true;
    return static_cast<u32>(std::clamp<s64>(value, 0, max));
}

// Applies `f` to each pair of elements of a and b.
template <typename F>
Vector elementwise(Width width, const Vector& a, const Vector& b, F f) {
    Vector result;
    for (u32 i = 0; i < count(width); ++i) {
        set(result, width, i, f(get(a, width, i), get(b, width, i)));
    }
    return result;
}

inline const Vector& va(const Thread& t, Instruction op) {
    return t.state.vr[op.va()];
}
inline const Vector& vb(const Thread& t, Instruction op) {
    return t.state.vr[op.vb()];
}
inline const Vector& vc(const Thread& t, Instruction op) {
    return t.state.vr[op.vc()];
}
inline void set_vd(Thread& t, Instruction op, const Vector& value) {
    t.state.vr[op.vd()] = value;
}

// SAT is sticky: saturating instructions only ever set it.
inline void set_saturated(Thread& t, bool saturated) {
    if (saturated) t.state.vscr |= k_vscr_saturated;
}

// The record form of a vector compare sets CR6: bit 0 if every element compared true, bit 2 if
// none did. The result elements are all ones (true) or all zeros (false).
inline void record_compare(Thread& t, Instruction op, const Vector& result) {
    if (!op.vrc()) return;
    const bool all_true = result.high() == ~u64{0} && result.low() == ~u64{0};
    const bool all_false = result.high() == 0 && result.low() == 0;
    t.state.set_cr_field(6, (all_true ? 0b1000 : 0) | (all_false ? 0b0010 : 0));
}

} // namespace ppu::vmx
