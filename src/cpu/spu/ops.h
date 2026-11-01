#pragma once

// Helpers shared by the SPU instruction files: operand access and element-wise operations.

#include "cpu/spu/opcodes.h"

namespace spu::ops {

inline const Vector& ra(const Thread& t, Instruction op) {
    return t.state.gpr[op.ra()];
}
inline const Vector& rb(const Thread& t, Instruction op) {
    return t.state.gpr[op.rb()];
}
// RC exists only in the RRR form, where it occupies the usual RT position.
inline const Vector& rc(const Thread& t, Instruction op) {
    return t.state.gpr[op.rc()];
}
// RT as a source, for instructions that also read their target (iohl, addx, dfma, brz, ...).
inline const Vector& rt(const Thread& t, Instruction op) {
    return t.state.gpr[op.rt()];
}

inline void set_rt(Thread& t, Instruction op, const Vector& value) {
    t.state.gpr[op.rt()] = value;
}
// The RRR form's target, in bits 4-10.
inline void set_rrr_rt(Thread& t, Instruction op, const Vector& value) {
    t.state.gpr[op.rrr_rt()] = value;
}

// A value in the preferred word slot, with the rest of the register zero.
inline Vector preferred_word(u32 value) {
    return Vector::from_words(value, 0, 0, 0);
}

inline Vector splat_word(u32 value) {
    return Vector::from_words(value, value, value, value);
}
inline Vector splat_half(u16 value) {
    Vector v;
    for (u32 i = 0; i < 8; ++i) {
        v.set_half(i, value);
    }
    return v;
}
inline Vector splat_byte(u8 value) {
    Vector v;
    v.bytes.fill(value);
    return v;
}

// Apply `f` to each pair of elements of a and b. The result is truncated to the element size.
template <typename F>
Vector each_word(const Vector& a, const Vector& b, F f) {
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        result.set_word(i, static_cast<u32>(f(a.word(i), b.word(i))));
    }
    return result;
}

template <typename F>
Vector each_half(const Vector& a, const Vector& b, F f) {
    Vector result;
    for (u32 i = 0; i < 8; ++i) {
        result.set_half(i, static_cast<u16>(f(a.half(i), b.half(i))));
    }
    return result;
}

template <typename F>
Vector each_byte(const Vector& a, const Vector& b, F f) {
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        result.set_byte(i, static_cast<u8>(f(a.byte(i), b.byte(i))));
    }
    return result;
}

// All ones for true, all zeros for false, as compare results are.
constexpr u32 mask32(bool condition) {
    return condition ? 0xffff'ffff : 0;
}
constexpr u16 mask16(bool condition) {
    return condition ? 0xffff : 0;
}
constexpr u8 mask8(bool condition) {
    return condition ? 0xff : 0;
}

} // namespace spu::ops
