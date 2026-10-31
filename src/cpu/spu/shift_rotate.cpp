// Shift and rotate instructions.
// Reference: SPU ISA v1.2, chapter 6 "Shift and Rotate Instructions".
//
// There are no right-shift instructions as such. "Rotate and mask" shifts right by the two's
// complement of its count, so `rotmi rt, ra, -n` shifts right by n. Each instruction masks its
// count to a particular width; the masks below are the manual's.

#include "common/u128.h"
#include "cpu/spu/ops.h"

#include <algorithm>
#include <bit>

namespace spu {

namespace {

using namespace ops;

// ---- Element shifts ----

// Applies `f(element, count)` to each halfword or word, with counts from RB or an immediate.
template <typename F>
void each_half_by_rb(Thread& t, Instruction op, F f) {
    set_rt(t, op, each_half(ra(t, op), rb(t, op), f));
}
template <typename F>
void each_half_by_immediate(Thread& t, Instruction op, F f) {
    set_rt(t, op, each_half(ra(t, op), splat_half(static_cast<u16>(op.i7())), f));
}
template <typename F>
void each_word_by_rb(Thread& t, Instruction op, F f) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), f));
}
template <typename F>
void each_word_by_immediate(Thread& t, Instruction op, F f) {
    set_rt(t, op, each_word(ra(t, op), splat_word(static_cast<u32>(op.i7())), f));
}

// Shift left: counts of 16 (32) or more give zero.
u16 shift_left_half(u16 a, u16 count) {
    const u32 s = count & 0x1f;
    return s < 16 ? static_cast<u16>(a << s) : 0;
}
u32 shift_left_word(u32 a, u32 count) {
    const u32 s = count & 0x3f;
    return s < 32 ? a << s : 0;
}
u16 rotate_half(u16 a, u16 count) {
    return std::rotl(a, count & 0xf);
}
u32 rotate_word(u32 a, u32 count) {
    return std::rotl(a, static_cast<int>(count & 0x1f));
}
// Rotate and mask: a logical right shift by (0 - count).
u16 shift_right_half(u16 a, u16 count) {
    const u32 s = (0u - count) & 0x1f;
    return s < 16 ? static_cast<u16>(a >> s) : 0;
}
u32 shift_right_word(u32 a, u32 count) {
    const u32 s = (0u - count) & 0x3f;
    return s < 32 ? a >> s : 0;
}
// Rotate and mask algebraic: an arithmetic right shift by (0 - count). Large counts fill the
// element with its sign bit.
u16 shift_right_algebraic_half(u16 a, u16 count) {
    const u32 s = std::min((0u - count) & 0x1f, 15u);
    return static_cast<u16>(static_cast<s16>(a) >> s);
}
u32 shift_right_algebraic_word(u32 a, u32 count) {
    const u32 s = std::min((0u - count) & 0x3f, 31u);
    return static_cast<u32>(static_cast<s32>(a) >> s);
}

void shlh(Thread& t, Instruction op) {
    each_half_by_rb(t, op, shift_left_half);
}
void shlhi(Thread& t, Instruction op) {
    each_half_by_immediate(t, op, shift_left_half);
}
void shl(Thread& t, Instruction op) {
    each_word_by_rb(t, op, shift_left_word);
}
void shli(Thread& t, Instruction op) {
    each_word_by_immediate(t, op, shift_left_word);
}
void roth(Thread& t, Instruction op) {
    each_half_by_rb(t, op, rotate_half);
}
void rothi(Thread& t, Instruction op) {
    each_half_by_immediate(t, op, rotate_half);
}
void rot(Thread& t, Instruction op) {
    each_word_by_rb(t, op, rotate_word);
}
void roti(Thread& t, Instruction op) {
    each_word_by_immediate(t, op, rotate_word);
}
void rothm(Thread& t, Instruction op) {
    each_half_by_rb(t, op, shift_right_half);
}
void rothmi(Thread& t, Instruction op) {
    each_half_by_immediate(t, op, shift_right_half);
}
void rotm(Thread& t, Instruction op) {
    each_word_by_rb(t, op, shift_right_word);
}
void rotmi(Thread& t, Instruction op) {
    each_word_by_immediate(t, op, shift_right_word);
}
void rotmah(Thread& t, Instruction op) {
    each_half_by_rb(t, op, shift_right_algebraic_half);
}
void rotmahi(Thread& t, Instruction op) {
    each_half_by_immediate(t, op, shift_right_algebraic_half);
}
void rotma(Thread& t, Instruction op) {
    each_word_by_rb(t, op, shift_right_algebraic_word);
}
void rotmai(Thread& t, Instruction op) {
    each_word_by_immediate(t, op, shift_right_algebraic_word);
}

// ---- Quadword shifts ----
// The whole register as one 128-bit number. Counts are in bits (0-7) or bytes (0-31); byte
// shifts of 16 or more give zero.

u128 to_u128(const Vector& v) {
    return (u128{v.high()} << 64) | v.low();
}
Vector from_u128(u128 x) {
    return Vector::from_dwords(static_cast<u64>(x >> 64), static_cast<u64>(x));
}

void shift_quad_left(Thread& t, Instruction op, u32 bits) {
    const u128 a = to_u128(ra(t, op));
    set_rt(t, op, from_u128(bits < 128 ? a << bits : 0));
}
void shift_quad_right(Thread& t, Instruction op, u32 bits) {
    const u128 a = to_u128(ra(t, op));
    set_rt(t, op, from_u128(bits < 128 ? a >> bits : 0));
}
void rotate_quad(Thread& t, Instruction op, u32 bits) {
    const u128 a = to_u128(ra(t, op));
    bits %= 128;
    set_rt(t, op, from_u128(bits == 0 ? a : (a << bits) | (a >> (128 - bits))));
}

// Counts from the preferred slot of RB.
u32 rb_bits(const Thread& t, Instruction op) {
    return t.preferred(op.rb()) & 0x7;
}
u32 rb_bytes(const Thread& t, Instruction op) {
    return t.preferred(op.rb()) & 0x1f;
}
// The "bytes from bit shift count" forms take bits 24-28: the bit count divided by 8.
u32 rb_bytes_from_bits(const Thread& t, Instruction op) {
    return (t.preferred(op.rb()) >> 3) & 0x1f;
}
u32 immediate(Instruction op) {
    return static_cast<u32>(op.i7());
}

void shlqbi(Thread& t, Instruction op) {
    shift_quad_left(t, op, rb_bits(t, op));
}
void shlqbii(Thread& t, Instruction op) {
    shift_quad_left(t, op, immediate(op) & 0x7);
}
void shlqby(Thread& t, Instruction op) {
    shift_quad_left(t, op, 8 * rb_bytes(t, op));
}
void shlqbyi(Thread& t, Instruction op) {
    shift_quad_left(t, op, 8 * (immediate(op) & 0x1f));
}
void shlqbybi(Thread& t, Instruction op) {
    shift_quad_left(t, op, 8 * rb_bytes_from_bits(t, op));
}
void rotqby(Thread& t, Instruction op) {
    rotate_quad(t, op, 8 * (rb_bytes(t, op) & 0xf));
}
void rotqbyi(Thread& t, Instruction op) {
    rotate_quad(t, op, 8 * (immediate(op) & 0xf));
}
void rotqbybi(Thread& t, Instruction op) {
    rotate_quad(t, op, 8 * (rb_bytes_from_bits(t, op) & 0xf));
}
void rotqbi(Thread& t, Instruction op) {
    rotate_quad(t, op, rb_bits(t, op));
}
void rotqbii(Thread& t, Instruction op) {
    rotate_quad(t, op, immediate(op) & 0x7);
}
void rotqmby(Thread& t, Instruction op) {
    shift_quad_right(t, op, 8 * ((0u - t.preferred(op.rb())) & 0x1f));
}
void rotqmbyi(Thread& t, Instruction op) {
    shift_quad_right(t, op, 8 * ((0u - immediate(op)) & 0x1f));
}
void rotqmbybi(Thread& t, Instruction op) {
    shift_quad_right(t, op, 8 * ((0u - rb_bytes_from_bits(t, op)) & 0x1f));
}
void rotqmbi(Thread& t, Instruction op) {
    shift_quad_right(t, op, (0u - t.preferred(op.rb())) & 0x7);
}
void rotqmbii(Thread& t, Instruction op) {
    shift_quad_right(t, op, (0u - immediate(op)) & 0x7);
}

constexpr OpcodeInfo k_opcodes[] = {
    {"shlh", Form::RR, 0x5f, shlh, "RT, RA, RB"},
    {"shlhi", Form::RI7, 0x7f, shlhi, "RT, RA, I7"},
    {"shl", Form::RR, 0x5b, shl, "RT, RA, RB"},
    {"shli", Form::RI7, 0x7b, shli, "RT, RA, I7"},
    {"shlqbi", Form::RR, 0x1db, shlqbi, "RT, RA, RB"},
    {"shlqbii", Form::RI7, 0x1fb, shlqbii, "RT, RA, I7"},
    {"shlqby", Form::RR, 0x1df, shlqby, "RT, RA, RB"},
    {"shlqbyi", Form::RI7, 0x1ff, shlqbyi, "RT, RA, I7"},
    {"shlqbybi", Form::RR, 0x1cf, shlqbybi, "RT, RA, RB"},
    {"roth", Form::RR, 0x5c, roth, "RT, RA, RB"},
    {"rothi", Form::RI7, 0x7c, rothi, "RT, RA, I7"},
    {"rot", Form::RR, 0x58, rot, "RT, RA, RB"},
    {"roti", Form::RI7, 0x78, roti, "RT, RA, I7"},
    {"rotqby", Form::RR, 0x1dc, rotqby, "RT, RA, RB"},
    {"rotqbyi", Form::RI7, 0x1fc, rotqbyi, "RT, RA, I7"},
    {"rotqbybi", Form::RR, 0x1cc, rotqbybi, "RT, RA, RB"},
    {"rotqbi", Form::RR, 0x1d8, rotqbi, "RT, RA, RB"},
    {"rotqbii", Form::RI7, 0x1f8, rotqbii, "RT, RA, I7"},
    {"rothm", Form::RR, 0x5d, rothm, "RT, RA, RB"},
    {"rothmi", Form::RI7, 0x7d, rothmi, "RT, RA, I7"},
    {"rotm", Form::RR, 0x59, rotm, "RT, RA, RB"},
    {"rotmi", Form::RI7, 0x79, rotmi, "RT, RA, I7"},
    {"rotqmby", Form::RR, 0x1dd, rotqmby, "RT, RA, RB"},
    {"rotqmbyi", Form::RI7, 0x1fd, rotqmbyi, "RT, RA, I7"},
    {"rotqmbybi", Form::RR, 0x1cd, rotqmbybi, "RT, RA, RB"},
    {"rotqmbi", Form::RR, 0x1d9, rotqmbi, "RT, RA, RB"},
    {"rotqmbii", Form::RI7, 0x1f9, rotqmbii, "RT, RA, I7"},
    {"rotmah", Form::RR, 0x5e, rotmah, "RT, RA, RB"},
    {"rotmahi", Form::RI7, 0x7e, rotmahi, "RT, RA, I7"},
    {"rotma", Form::RR, 0x5a, rotma, "RT, RA, RB"},
    {"rotmai", Form::RI7, 0x7a, rotmai, "RT, RA, I7"},
};

} // namespace

std::span<const OpcodeInfo> shift_rotate_opcodes() {
    return k_opcodes;
}

} // namespace spu
