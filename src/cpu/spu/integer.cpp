// Integer and logical instructions.
// Reference: SPU ISA v1.2, chapter 5 "Integer and Logical Instructions".

#include "cpu/spu/ops.h"

#include <bit>

namespace spu {

namespace {

using namespace ops;

// Immediates are sign-extended to the element size, then used as unsigned bit patterns.
Vector immediate_words(Instruction op) {
    return splat_word(static_cast<u32>(op.i10()));
}
Vector immediate_halves(Instruction op) {
    return splat_half(static_cast<u16>(op.i10()));
}
Vector immediate_bytes(Instruction op) {
    return splat_byte(static_cast<u8>(op.i10()));
}

// ---- Add and subtract ----

u32 add(u32 a, u32 b) {
    return a + b;
}
// "Subtract from": the result is b - a.
u32 subtract_from(u32 a, u32 b) {
    return b - a;
}

void ah(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), rb(t, op), add));
}
void ahi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), add));
}
void a(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), add));
}
void ai(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), add));
}
void sfh(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), rb(t, op), subtract_from));
}
void sfhi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), subtract_from));
}
void sf(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), subtract_from));
}
void sfi(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), subtract_from));
}

// ---- Extended arithmetic ----
// Wider additions chain through the least significant bit of each word of RT: the carry (or
// "no borrow") from the previous step.

// Applies `f(a, b, rt)` to each word, where rt is the target's old value.
template <typename F>
void each_word_with_rt(Thread& t, Instruction op, F f) {
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        result.set_word(i, f(ra(t, op).word(i), rb(t, op).word(i), rt(t, op).word(i)));
    }
    set_rt(t, op, result);
}

void addx(Thread& t, Instruction op) {
    each_word_with_rt(t, op, [](u32 a, u32 b, u32 c) { return a + b + (c & 1); });
}
void sfx(Thread& t, Instruction op) {
    each_word_with_rt(t, op, [](u32 a, u32 b, u32 c) { return b + ~a + (c & 1); });
}
void cg(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return static_cast<u32>((u64{a} + b) >> 32);
           }));
}
void cgx(Thread& t, Instruction op) {
    each_word_with_rt(
        t, op, [](u32 a, u32 b, u32 c) { return static_cast<u32>((u64{a} + b + (c & 1)) >> 32); });
}
// Borrow generate: 1 if b - a needs no borrow.
void bg(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return b >= a ? 1 : 0; }));
}
void bgx(Thread& t, Instruction op) {
    each_word_with_rt(t, op, [](u32 a, u32 b, u32 c) -> u32 { return (c & 1) ? b >= a : b > a; });
}

// ---- Multiply ----
// The multiplier is 16 x 16 bits. "Low" is the right halfword of each word, "high" the left.

s32 low_signed(u32 word) {
    return static_cast<s16>(word);
}
u32 low_unsigned(u32 word) {
    return word & 0xffff;
}
s32 high_signed(u32 word) {
    return static_cast<s16>(word >> 16);
}
u32 high_unsigned(u32 word) {
    return word >> 16;
}

u32 multiply_low_signed(u32 a, u32 b) {
    return static_cast<u32>(low_signed(a) * low_signed(b));
}
u32 multiply_low_unsigned(u32 a, u32 b) {
    return low_unsigned(a) * low_unsigned(b);
}
u32 multiply_high_signed(u32 a, u32 b) {
    return static_cast<u32>(high_signed(a) * high_signed(b));
}
u32 multiply_high_unsigned(u32 a, u32 b) {
    return high_unsigned(a) * high_unsigned(b);
}

void mpy(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), multiply_low_signed));
}
void mpyu(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), multiply_low_unsigned));
}
void mpyi(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), multiply_low_signed));
}
// The immediate is sign-extended to 16 bits, then treated as unsigned.
void mpyui(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), multiply_low_unsigned));
}
void mpya(Thread& t, Instruction op) {
    const Vector product = each_word(ra(t, op), rb(t, op), multiply_low_signed);
    set_rrr_rt(t, op, each_word(product, rc(t, op), add));
}
// The low 16 bits of high(a) * low(b), shifted into the high halfword.
void mpyh(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return (high_unsigned(a) * low_unsigned(b)) << 16;
           }));
}
// The signed product shifted right 16 bits, sign-extended.
void mpys(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return static_cast<u32>((low_signed(a) * low_signed(b)) >> 16);
           }));
}
void mpyhh(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), multiply_high_signed));
}
void mpyhhu(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), multiply_high_unsigned));
}
void mpyhha(Thread& t, Instruction op) {
    each_word_with_rt(t, op, [](u32 a, u32 b, u32 c) { return multiply_high_signed(a, b) + c; });
}
void mpyhhau(Thread& t, Instruction op) {
    each_word_with_rt(t, op, [](u32 a, u32 b, u32 c) { return multiply_high_unsigned(a, b) + c; });
}

// ---- Bit counting, masks and gathering ----

void clz(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), {}, [](u32 a, u32) { return std::countl_zero(a); }));
}
void cntb(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), {}, [](u8 a, u8) { return std::popcount(a); }));
}

// Form select mask: each of the low `count` bits of the preferred slot, from the most
// significant, becomes an element of all ones or all zeros.
void form_select_mask(Thread& t, Instruction op, u32 count) {
    const u32 bits = t.preferred(op.ra());
    const u32 size = 16 / count;
    Vector result;
    for (u32 i = 0; i < count; ++i) {
        const u8 fill = mask8((bits >> (count - 1 - i)) & 1);
        for (u32 j = 0; j < size; ++j) {
            result.set_byte(i * size + j, fill);
        }
    }
    set_rt(t, op, result);
}

void fsmb(Thread& t, Instruction op) {
    form_select_mask(t, op, 16);
}
void fsmh(Thread& t, Instruction op) {
    form_select_mask(t, op, 8);
}
void fsm(Thread& t, Instruction op) {
    form_select_mask(t, op, 4);
}

// Gather bits: the least significant bit of each element, packed into the preferred slot.
void gather_bits(Thread& t, Instruction op, u32 count) {
    const u32 size = 16 / count;
    const Vector& source = ra(t, op);
    u32 bits = 0;
    for (u32 i = 0; i < count; ++i) {
        bits = (bits << 1) | (source.byte(i * size + size - 1) & 1);
    }
    set_rt(t, op, preferred_word(bits));
}

void gbb(Thread& t, Instruction op) {
    gather_bits(t, op, 16);
}
void gbh(Thread& t, Instruction op) {
    gather_bits(t, op, 8);
}
void gb(Thread& t, Instruction op) {
    gather_bits(t, op, 4);
}

// ---- Byte arithmetic ----

void avgb(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), rb(t, op), [](u8 a, u8 b) { return (a + b + 1) >> 1; }));
}
void absdb(Thread& t, Instruction op) {
    set_rt(t, op,
           each_byte(ra(t, op), rb(t, op), [](u8 a, u8 b) { return a > b ? a - b : b - a; }));
}
// Each word gets the sum of RB's four bytes in its high halfword and RA's in its low halfword.
void sumb(Thread& t, Instruction op) {
    auto sum_of_bytes = [](u32 word) {
        return (word >> 24) + ((word >> 16) & 0xff) + ((word >> 8) & 0xff) + (word & 0xff);
    };
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [&](u32 a, u32 b) {
               return (sum_of_bytes(b) << 16) | sum_of_bytes(a);
           }));
}

// ---- Sign extension ----

void xsbh(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), {}, [](u16 a, u16) { return static_cast<s8>(a); }));
}
void xshw(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), {}, [](u32 a, u32) { return static_cast<s16>(a); }));
}
void xswd(Thread& t, Instruction op) {
    const Vector& source = ra(t, op);
    auto extend = [&](u32 i) { return static_cast<u64>(static_cast<s32>(source.dword(i))); };
    set_rt(t, op, Vector::from_dwords(extend(0), extend(1)));
}

// ---- Logical ----

void and_(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return a & b; }));
}
void andc(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return a & ~b; }));
}
void andbi(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), immediate_bytes(op), [](u8 a, u8 b) { return a & b; }));
}
void andhi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), [](u16 a, u16 b) { return a & b; }));
}
void andi(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), [](u32 a, u32 b) { return a & b; }));
}
void or_(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return a | b; }));
}
void orc(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return a | ~b; }));
}
void orbi(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), immediate_bytes(op), [](u8 a, u8 b) { return a | b; }));
}
void orhi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), [](u16 a, u16 b) { return a | b; }));
}
void ori(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), [](u32 a, u32 b) { return a | b; }));
}
// Or across: the four words ORed together, in the preferred slot.
void orx(Thread& t, Instruction op) {
    const Vector& v = ra(t, op);
    set_rt(t, op, preferred_word(v.word(0) | v.word(1) | v.word(2) | v.word(3)));
}
void xor_(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return a ^ b; }));
}
void xorbi(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), immediate_bytes(op), [](u8 a, u8 b) { return a ^ b; }));
}
void xorhi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), [](u16 a, u16 b) { return a ^ b; }));
}
void xori(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), [](u32 a, u32 b) { return a ^ b; }));
}
void nand(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return ~(a & b); }));
}
void nor(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return ~(a | b); }));
}
void eqv(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) { return ~(a ^ b); }));
}

// ---- Select and shuffle ----

// Each bit comes from RB where RC has a 1, and from RA where it has a 0.
void selb(Thread& t, Instruction op) {
    const Vector from_b = each_word(rb(t, op), rc(t, op), [](u32 b, u32 c) { return b & c; });
    const Vector from_a = each_word(ra(t, op), rc(t, op), [](u32 a, u32 c) { return a & ~c; });
    set_rrr_rt(t, op, each_word(from_a, from_b, [](u32 a, u32 b) { return a | b; }));
}

// Each byte of RC picks a byte of RA:RB (0-31), or one of three constants. Table 5-1.
void shufb(Thread& t, Instruction op) {
    const Vector& a = ra(t, op);
    const Vector& b = rb(t, op);
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        const u8 control = rc(t, op).byte(i);
        u8 value;
        if ((control & 0xc0) == 0x80) {
            value = 0x00;
        } else if ((control & 0xe0) == 0xc0) {
            value = 0xff;
        } else if ((control & 0xe0) == 0xe0) {
            value = 0x80;
        } else {
            const u32 index = control & 0x1f;
            value = index < 16 ? a.byte(index) : b.byte(index - 16);
        }
        result.set_byte(i, value);
    }
    set_rrr_rt(t, op, result);
}

constexpr OpcodeInfo k_opcodes[] = {
    {"ah", Form::RR, 0xc8, ah, "RT, RA, RB"},
    {"ahi", Form::RI10, 0x1d, ahi, "RT, RA, I10"},
    {"a", Form::RR, 0xc0, a, "RT, RA, RB"},
    {"ai", Form::RI10, 0x1c, ai, "RT, RA, I10"},
    {"sfh", Form::RR, 0x48, sfh, "RT, RA, RB"},
    {"sfhi", Form::RI10, 0x0d, sfhi, "RT, RA, I10"},
    {"sf", Form::RR, 0x40, sf, "RT, RA, RB"},
    {"sfi", Form::RI10, 0x0c, sfi, "RT, RA, I10"},
    {"addx", Form::RR, 0x340, addx, "RT, RA, RB"},
    {"cg", Form::RR, 0xc2, cg, "RT, RA, RB"},
    {"cgx", Form::RR, 0x342, cgx, "RT, RA, RB"},
    {"sfx", Form::RR, 0x341, sfx, "RT, RA, RB"},
    {"bg", Form::RR, 0x42, bg, "RT, RA, RB"},
    {"bgx", Form::RR, 0x343, bgx, "RT, RA, RB"},
    {"mpy", Form::RR, 0x3c4, mpy, "RT, RA, RB"},
    {"mpyu", Form::RR, 0x3cc, mpyu, "RT, RA, RB"},
    {"mpyi", Form::RI10, 0x74, mpyi, "RT, RA, I10"},
    {"mpyui", Form::RI10, 0x75, mpyui, "RT, RA, I10"},
    {"mpya", Form::RRR, 0xc, mpya, "RT, RA, RB, RC"},
    {"mpyh", Form::RR, 0x3c5, mpyh, "RT, RA, RB"},
    {"mpys", Form::RR, 0x3c7, mpys, "RT, RA, RB"},
    {"mpyhh", Form::RR, 0x3c6, mpyhh, "RT, RA, RB"},
    {"mpyhha", Form::RR, 0x346, mpyhha, "RT, RA, RB"},
    {"mpyhhu", Form::RR, 0x3ce, mpyhhu, "RT, RA, RB"},
    {"mpyhhau", Form::RR, 0x34e, mpyhhau, "RT, RA, RB"},
    {"clz", Form::RR, 0x2a5, clz, "RT, RA"},
    {"cntb", Form::RR, 0x2b4, cntb, "RT, RA"},
    {"fsmb", Form::RR, 0x1b6, fsmb, "RT, RA"},
    {"fsmh", Form::RR, 0x1b5, fsmh, "RT, RA"},
    {"fsm", Form::RR, 0x1b4, fsm, "RT, RA"},
    {"gbb", Form::RR, 0x1b2, gbb, "RT, RA"},
    {"gbh", Form::RR, 0x1b1, gbh, "RT, RA"},
    {"gb", Form::RR, 0x1b0, gb, "RT, RA"},
    {"avgb", Form::RR, 0xd3, avgb, "RT, RA, RB"},
    {"absdb", Form::RR, 0x53, absdb, "RT, RA, RB"},
    {"sumb", Form::RR, 0x253, sumb, "RT, RA, RB"},
    {"xsbh", Form::RR, 0x2b6, xsbh, "RT, RA"},
    {"xshw", Form::RR, 0x2ae, xshw, "RT, RA"},
    {"xswd", Form::RR, 0x2a6, xswd, "RT, RA"},
    {"and", Form::RR, 0xc1, and_, "RT, RA, RB"},
    {"andc", Form::RR, 0x2c1, andc, "RT, RA, RB"},
    {"andbi", Form::RI10, 0x16, andbi, "RT, RA, I10"},
    {"andhi", Form::RI10, 0x15, andhi, "RT, RA, I10"},
    {"andi", Form::RI10, 0x14, andi, "RT, RA, I10"},
    {"or", Form::RR, 0x41, or_, "RT, RA, RB"},
    {"orc", Form::RR, 0x2c9, orc, "RT, RA, RB"},
    {"orbi", Form::RI10, 0x06, orbi, "RT, RA, I10"},
    {"orhi", Form::RI10, 0x05, orhi, "RT, RA, I10"},
    {"ori", Form::RI10, 0x04, ori, "RT, RA, I10"},
    {"orx", Form::RR, 0x1f0, orx, "RT, RA"},
    {"xor", Form::RR, 0x241, xor_, "RT, RA, RB"},
    {"xorbi", Form::RI10, 0x46, xorbi, "RT, RA, I10"},
    {"xorhi", Form::RI10, 0x45, xorhi, "RT, RA, I10"},
    {"xori", Form::RI10, 0x44, xori, "RT, RA, I10"},
    {"nand", Form::RR, 0xc9, nand, "RT, RA, RB"},
    {"nor", Form::RR, 0x49, nor, "RT, RA, RB"},
    {"eqv", Form::RR, 0x249, eqv, "RT, RA, RB"},
    {"selb", Form::RRR, 0x8, selb, "RT, RA, RB, RC"},
    {"shufb", Form::RRR, 0xb, shufb, "RT, RA, RB, RC"},
};

} // namespace

std::span<const OpcodeInfo> integer_opcodes() {
    return k_opcodes;
}

} // namespace spu
