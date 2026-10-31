// Vector integer instructions: arithmetic, logic, shifts, multiplies, sums and compares.
// Reference: Power ISA v3.1, Book I, chapter 6, "Vector Facility". "Modulo" instructions wrap;
// "saturate" instructions clamp to the element's range and set VSCR[SAT].

#include "cpu/ppu/vector_ops.h"

namespace ppu {

namespace {

using namespace vmx;

// ---- Add and subtract ----

void add_modulo(Thread& t, Instruction op, Width w) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [](u32 a, u32 b) { return a + b; }));
}

void subtract_modulo(Thread& t, Instruction op, Width w) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [](u32 a, u32 b) { return a - b; }));
}

void add_saturate(Thread& t, Instruction op, Width w, bool is_signed) {
    bool saturated = false;
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               return is_signed ? saturate_signed(as_signed(a, w) + as_signed(b, w), w, saturated)
                                : saturate_unsigned(s64{a} + b, w, saturated);
           }));
    set_saturated(t, saturated);
}

void subtract_saturate(Thread& t, Instruction op, Width w, bool is_signed) {
    bool saturated = false;
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               return is_signed ? saturate_signed(as_signed(a, w) - as_signed(b, w), w, saturated)
                                : saturate_unsigned(s64{a} - b, w, saturated);
           }));
    set_saturated(t, saturated);
}

void vaddubm(Thread& t, Instruction op) {
    add_modulo(t, op, Width::byte);
}
void vadduhm(Thread& t, Instruction op) {
    add_modulo(t, op, Width::half);
}
void vadduwm(Thread& t, Instruction op) {
    add_modulo(t, op, Width::word);
}
void vaddubs(Thread& t, Instruction op) {
    add_saturate(t, op, Width::byte, false);
}
void vadduhs(Thread& t, Instruction op) {
    add_saturate(t, op, Width::half, false);
}
void vadduws(Thread& t, Instruction op) {
    add_saturate(t, op, Width::word, false);
}
void vaddsbs(Thread& t, Instruction op) {
    add_saturate(t, op, Width::byte, true);
}
void vaddshs(Thread& t, Instruction op) {
    add_saturate(t, op, Width::half, true);
}
void vaddsws(Thread& t, Instruction op) {
    add_saturate(t, op, Width::word, true);
}
void vsububm(Thread& t, Instruction op) {
    subtract_modulo(t, op, Width::byte);
}
void vsubuhm(Thread& t, Instruction op) {
    subtract_modulo(t, op, Width::half);
}
void vsubuwm(Thread& t, Instruction op) {
    subtract_modulo(t, op, Width::word);
}
void vsububs(Thread& t, Instruction op) {
    subtract_saturate(t, op, Width::byte, false);
}
void vsubuhs(Thread& t, Instruction op) {
    subtract_saturate(t, op, Width::half, false);
}
void vsubuws(Thread& t, Instruction op) {
    subtract_saturate(t, op, Width::word, false);
}
void vsubsbs(Thread& t, Instruction op) {
    subtract_saturate(t, op, Width::byte, true);
}
void vsubshs(Thread& t, Instruction op) {
    subtract_saturate(t, op, Width::half, true);
}
void vsubsws(Thread& t, Instruction op) {
    subtract_saturate(t, op, Width::word, true);
}

// The carry out of an unsigned word add, or "no borrow" for a subtract: 1 or 0 per word.
void vaddcuw(Thread& t, Instruction op) {
    set_vd(t, op, elementwise(Width::word, va(t, op), vb(t, op), [](u32 a, u32 b) {
               return (u64{a} + b) >> 32;
           }));
}

void vsubcuw(Thread& t, Instruction op) {
    set_vd(t, op, elementwise(Width::word, va(t, op), vb(t, op), [](u32 a, u32 b) {
               return a >= b ? 1u : 0u;
           }));
}

// ---- Maximum, minimum, average ----

void maximum(Thread& t, Instruction op, Width w, bool is_signed) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               const bool a_larger = is_signed ? as_signed(a, w) > as_signed(b, w) : a > b;
               return a_larger ? a : b;
           }));
}

void minimum(Thread& t, Instruction op, Width w, bool is_signed) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               const bool a_smaller = is_signed ? as_signed(a, w) < as_signed(b, w) : a < b;
               return a_smaller ? a : b;
           }));
}

// (a + b + 1) / 2, computed without overflow; signed averages round toward +infinity too.
void average(Thread& t, Instruction op, Width w, bool is_signed) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               const s64 sum = is_signed ? as_signed(a, w) + as_signed(b, w) + 1 : s64{a} + b + 1;
               return static_cast<u32>(sum >> 1);
           }));
}

void vmaxub(Thread& t, Instruction op) {
    maximum(t, op, Width::byte, false);
}
void vmaxuh(Thread& t, Instruction op) {
    maximum(t, op, Width::half, false);
}
void vmaxuw(Thread& t, Instruction op) {
    maximum(t, op, Width::word, false);
}
void vmaxsb(Thread& t, Instruction op) {
    maximum(t, op, Width::byte, true);
}
void vmaxsh(Thread& t, Instruction op) {
    maximum(t, op, Width::half, true);
}
void vmaxsw(Thread& t, Instruction op) {
    maximum(t, op, Width::word, true);
}
void vminub(Thread& t, Instruction op) {
    minimum(t, op, Width::byte, false);
}
void vminuh(Thread& t, Instruction op) {
    minimum(t, op, Width::half, false);
}
void vminuw(Thread& t, Instruction op) {
    minimum(t, op, Width::word, false);
}
void vminsb(Thread& t, Instruction op) {
    minimum(t, op, Width::byte, true);
}
void vminsh(Thread& t, Instruction op) {
    minimum(t, op, Width::half, true);
}
void vminsw(Thread& t, Instruction op) {
    minimum(t, op, Width::word, true);
}
void vavgub(Thread& t, Instruction op) {
    average(t, op, Width::byte, false);
}
void vavguh(Thread& t, Instruction op) {
    average(t, op, Width::half, false);
}
void vavguw(Thread& t, Instruction op) {
    average(t, op, Width::word, false);
}
void vavgsb(Thread& t, Instruction op) {
    average(t, op, Width::byte, true);
}
void vavgsh(Thread& t, Instruction op) {
    average(t, op, Width::half, true);
}
void vavgsw(Thread& t, Instruction op) {
    average(t, op, Width::word, true);
}

// ---- Rotate and shift ----
// Each element of b gives the shift for the matching element of a, modulo the element size.

void rotate_left(Thread& t, Instruction op, Width w) {
    const u32 bits = bit_size(w);
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               const u32 n = b % bits;
               return n == 0 ? a : (a << n) | (a >> (bits - n));
           }));
}

void shift_left(Thread& t, Instruction op, Width w) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               return a << (b % bit_size(w));
           }));
}

void shift_right(Thread& t, Instruction op, Width w) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               return a >> (b % bit_size(w));
           }));
}

void shift_right_algebraic(Thread& t, Instruction op, Width w) {
    set_vd(t, op, elementwise(w, va(t, op), vb(t, op), [&](u32 a, u32 b) {
               return static_cast<u32>(as_signed(a, w) >> (b % bit_size(w)));
           }));
}

void vrlb(Thread& t, Instruction op) {
    rotate_left(t, op, Width::byte);
}
void vrlh(Thread& t, Instruction op) {
    rotate_left(t, op, Width::half);
}
void vrlw(Thread& t, Instruction op) {
    rotate_left(t, op, Width::word);
}
void vslb(Thread& t, Instruction op) {
    shift_left(t, op, Width::byte);
}
void vslh(Thread& t, Instruction op) {
    shift_left(t, op, Width::half);
}
void vslw(Thread& t, Instruction op) {
    shift_left(t, op, Width::word);
}
void vsrb(Thread& t, Instruction op) {
    shift_right(t, op, Width::byte);
}
void vsrh(Thread& t, Instruction op) {
    shift_right(t, op, Width::half);
}
void vsrw(Thread& t, Instruction op) {
    shift_right(t, op, Width::word);
}
void vsrab(Thread& t, Instruction op) {
    shift_right_algebraic(t, op, Width::byte);
}
void vsrah(Thread& t, Instruction op) {
    shift_right_algebraic(t, op, Width::half);
}
void vsraw(Thread& t, Instruction op) {
    shift_right_algebraic(t, op, Width::word);
}

// ---- Logical ----

void logical(Thread& t, Instruction op, u32 (*f)(u32, u32)) {
    set_vd(t, op, elementwise(Width::word, va(t, op), vb(t, op), f));
}

void vand(Thread& t, Instruction op) {
    logical(t, op, [](u32 a, u32 b) { return a & b; });
}
void vandc(Thread& t, Instruction op) {
    logical(t, op, [](u32 a, u32 b) { return a & ~b; });
}
void vor(Thread& t, Instruction op) {
    logical(t, op, [](u32 a, u32 b) { return a | b; });
}
void vnor(Thread& t, Instruction op) {
    logical(t, op, [](u32 a, u32 b) { return ~(a | b); });
}
void vxor(Thread& t, Instruction op) {
    logical(t, op, [](u32 a, u32 b) { return a ^ b; });
}

// ---- Multiply ----

// Multiplies the even (0, 2, ...) or odd (1, 3, ...) elements, giving double-width products.
void multiply_alternate(Thread& t, Instruction op, Width w, bool is_signed, bool odd) {
    const Width product_width = static_cast<Width>(2 * static_cast<u32>(w));
    Vector result;
    for (u32 i = 0; i < count(product_width); ++i) {
        const u32 a = get(va(t, op), w, 2 * i + odd);
        const u32 b = get(vb(t, op), w, 2 * i + odd);
        const s64 product = is_signed ? as_signed(a, w) * as_signed(b, w) : s64{a} * b;
        set(result, product_width, i, static_cast<u32>(product));
    }
    set_vd(t, op, result);
}

void vmuleub(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::byte, false, false);
}
void vmuleuh(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::half, false, false);
}
void vmulesb(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::byte, true, false);
}
void vmulesh(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::half, true, false);
}
void vmuloub(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::byte, false, true);
}
void vmulouh(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::half, false, true);
}
void vmulosb(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::byte, true, true);
}
void vmulosh(Thread& t, Instruction op) {
    multiply_alternate(t, op, Width::half, true, true);
}

// vmhaddshs and vmhraddshs: the high half of a × b (optionally rounded), plus c, saturated.
void multiply_high_add(Thread& t, Instruction op, bool round) {
    bool saturated = false;
    Vector result;
    for (u32 i = 0; i < 8; ++i) {
        const s64 product =
            as_signed(va(t, op).half(i), Width::half) * as_signed(vb(t, op).half(i), Width::half);
        const s64 sum =
            ((product + (round ? 0x4000 : 0)) >> 15) + as_signed(vc(t, op).half(i), Width::half);
        result.set_half(i, static_cast<u16>(saturate_signed(sum, Width::half, saturated)));
    }
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

void vmhaddshs(Thread& t, Instruction op) {
    multiply_high_add(t, op, false);
}
void vmhraddshs(Thread& t, Instruction op) {
    multiply_high_add(t, op, true);
}

// vmladduhm: low half of a × b + c.
void vmladduhm(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 8; ++i) {
        const u32 value = u32{va(t, op).half(i)} * vb(t, op).half(i) + vc(t, op).half(i);
        result.set_half(i, static_cast<u16>(value));
    }
    set_vd(t, op, result);
}

// ---- Multiply-sum and sum across ----

// vmsum*: each word of the result is the sum of the products of the a and b elements in that
// word, plus the word from c. `a_signed`/`b_signed` say how each source's elements are read.
void multiply_sum(Thread& t, Instruction op, Width w, bool a_signed, bool b_signed, bool saturate) {
    const u32 per_word = 4 / static_cast<u32>(w);
    bool saturated = false;
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        s64 sum = (a_signed || b_signed) ? as_signed(vc(t, op).word(i), Width::word)
                                         : s64{vc(t, op).word(i)};
        for (u32 j = 0; j < per_word; ++j) {
            const u32 a = get(va(t, op), w, per_word * i + j);
            const u32 b = get(vb(t, op), w, per_word * i + j);
            sum += (a_signed ? as_signed(a, w) : s64{a}) * (b_signed ? as_signed(b, w) : s64{b});
        }
        u32 word = static_cast<u32>(sum);
        if (saturate) {
            word = a_signed ? saturate_signed(sum, Width::word, saturated)
                            : saturate_unsigned(sum, Width::word, saturated);
        }
        result.set_word(i, word);
    }
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

void vmsumubm(Thread& t, Instruction op) {
    multiply_sum(t, op, Width::byte, false, false, false);
}
void vmsummbm(Thread& t, Instruction op) {
    multiply_sum(t, op, Width::byte, true, false, false);
}
void vmsumuhm(Thread& t, Instruction op) {
    multiply_sum(t, op, Width::half, false, false, false);
}
void vmsumuhs(Thread& t, Instruction op) {
    multiply_sum(t, op, Width::half, false, false, true);
}
void vmsumshm(Thread& t, Instruction op) {
    multiply_sum(t, op, Width::half, true, true, false);
}
void vmsumshs(Thread& t, Instruction op) {
    multiply_sum(t, op, Width::half, true, true, true);
}

// vsum4*s: each word is the sum of the a elements in that word plus the word from b, saturated.
void sum_across_quarters(Thread& t, Instruction op, Width w, bool is_signed) {
    const u32 per_word = 4 / static_cast<u32>(w);
    bool saturated = false;
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        const u32 b = vb(t, op).word(i);
        s64 sum = is_signed ? as_signed(b, Width::word) : s64{b};
        for (u32 j = 0; j < per_word; ++j) {
            const u32 a = get(va(t, op), w, per_word * i + j);
            sum += is_signed ? as_signed(a, w) : s64{a};
        }
        result.set_word(i, is_signed ? saturate_signed(sum, Width::word, saturated)
                                     : saturate_unsigned(sum, Width::word, saturated));
    }
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

void vsum4ubs(Thread& t, Instruction op) {
    sum_across_quarters(t, op, Width::byte, false);
}
void vsum4sbs(Thread& t, Instruction op) {
    sum_across_quarters(t, op, Width::byte, true);
}
void vsum4shs(Thread& t, Instruction op) {
    sum_across_quarters(t, op, Width::half, true);
}

// vsum2sws: words 1 and 3 get the sum of their word pair from a plus the same word of b.
void vsum2sws(Thread& t, Instruction op) {
    bool saturated = false;
    Vector result;
    for (u32 pair = 0; pair < 2; ++pair) {
        const s64 sum = as_signed(va(t, op).word(2 * pair), Width::word) +
                        as_signed(va(t, op).word(2 * pair + 1), Width::word) +
                        as_signed(vb(t, op).word(2 * pair + 1), Width::word);
        result.set_word(2 * pair + 1, saturate_signed(sum, Width::word, saturated));
    }
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

// vsumsws: word 3 gets the sum of all of a's words plus word 3 of b.
void vsumsws(Thread& t, Instruction op) {
    bool saturated = false;
    s64 sum = as_signed(vb(t, op).word(3), Width::word);
    for (u32 i = 0; i < 4; ++i) {
        sum += as_signed(va(t, op).word(i), Width::word);
    }
    Vector result;
    result.set_word(3, saturate_signed(sum, Width::word, saturated));
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

// ---- Compare ----
// Each result element is all ones if the comparison is true, otherwise zero.

void compare(Thread& t, Instruction op, Width w, bool (*f)(u32, u32, Width)) {
    const Vector result = elementwise(w, va(t, op), vb(t, op),
                                      [&](u32 a, u32 b) { return f(a, b, w) ? ~u32{0} : 0u; });
    set_vd(t, op, result);
    record_compare(t, op, result);
}

bool equal(u32 a, u32 b, Width) {
    return a == b;
}
bool greater_unsigned(u32 a, u32 b, Width) {
    return a > b;
}
bool greater_signed(u32 a, u32 b, Width w) {
    return as_signed(a, w) > as_signed(b, w);
}

void vcmpequb(Thread& t, Instruction op) {
    compare(t, op, Width::byte, equal);
}
void vcmpequh(Thread& t, Instruction op) {
    compare(t, op, Width::half, equal);
}
void vcmpequw(Thread& t, Instruction op) {
    compare(t, op, Width::word, equal);
}
void vcmpgtub(Thread& t, Instruction op) {
    compare(t, op, Width::byte, greater_unsigned);
}
void vcmpgtuh(Thread& t, Instruction op) {
    compare(t, op, Width::half, greater_unsigned);
}
void vcmpgtuw(Thread& t, Instruction op) {
    compare(t, op, Width::word, greater_unsigned);
}
void vcmpgtsb(Thread& t, Instruction op) {
    compare(t, op, Width::byte, greater_signed);
}
void vcmpgtsh(Thread& t, Instruction op) {
    compare(t, op, Width::half, greater_signed);
}
void vcmpgtsw(Thread& t, Instruction op) {
    compare(t, op, Width::word, greater_signed);
}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic    form       op xo    handler      operands              flags
    {"vaddubm",    Form::VX,  4, 0,    vaddubm,     "vD, vA, vB"},
    {"vadduhm",    Form::VX,  4, 64,   vadduhm,     "vD, vA, vB"},
    {"vadduwm",    Form::VX,  4, 128,  vadduwm,     "vD, vA, vB"},
    {"vaddcuw",    Form::VX,  4, 384,  vaddcuw,     "vD, vA, vB"},
    {"vaddubs",    Form::VX,  4, 512,  vaddubs,     "vD, vA, vB"},
    {"vadduhs",    Form::VX,  4, 576,  vadduhs,     "vD, vA, vB"},
    {"vadduws",    Form::VX,  4, 640,  vadduws,     "vD, vA, vB"},
    {"vaddsbs",    Form::VX,  4, 768,  vaddsbs,     "vD, vA, vB"},
    {"vaddshs",    Form::VX,  4, 832,  vaddshs,     "vD, vA, vB"},
    {"vaddsws",    Form::VX,  4, 896,  vaddsws,     "vD, vA, vB"},
    {"vsububm",    Form::VX,  4, 1024, vsububm,     "vD, vA, vB"},
    {"vsubuhm",    Form::VX,  4, 1088, vsubuhm,     "vD, vA, vB"},
    {"vsubuwm",    Form::VX,  4, 1152, vsubuwm,     "vD, vA, vB"},
    {"vsubcuw",    Form::VX,  4, 1408, vsubcuw,     "vD, vA, vB"},
    {"vsububs",    Form::VX,  4, 1536, vsububs,     "vD, vA, vB"},
    {"vsubuhs",    Form::VX,  4, 1600, vsubuhs,     "vD, vA, vB"},
    {"vsubuws",    Form::VX,  4, 1664, vsubuws,     "vD, vA, vB"},
    {"vsubsbs",    Form::VX,  4, 1792, vsubsbs,     "vD, vA, vB"},
    {"vsubshs",    Form::VX,  4, 1856, vsubshs,     "vD, vA, vB"},
    {"vsubsws",    Form::VX,  4, 1920, vsubsws,     "vD, vA, vB"},
    {"vmaxub",     Form::VX,  4, 2,    vmaxub,      "vD, vA, vB"},
    {"vmaxuh",     Form::VX,  4, 66,   vmaxuh,      "vD, vA, vB"},
    {"vmaxuw",     Form::VX,  4, 130,  vmaxuw,      "vD, vA, vB"},
    {"vmaxsb",     Form::VX,  4, 258,  vmaxsb,      "vD, vA, vB"},
    {"vmaxsh",     Form::VX,  4, 322,  vmaxsh,      "vD, vA, vB"},
    {"vmaxsw",     Form::VX,  4, 386,  vmaxsw,      "vD, vA, vB"},
    {"vminub",     Form::VX,  4, 514,  vminub,      "vD, vA, vB"},
    {"vminuh",     Form::VX,  4, 578,  vminuh,      "vD, vA, vB"},
    {"vminuw",     Form::VX,  4, 642,  vminuw,      "vD, vA, vB"},
    {"vminsb",     Form::VX,  4, 770,  vminsb,      "vD, vA, vB"},
    {"vminsh",     Form::VX,  4, 834,  vminsh,      "vD, vA, vB"},
    {"vminsw",     Form::VX,  4, 898,  vminsw,      "vD, vA, vB"},
    {"vavgub",     Form::VX,  4, 1026, vavgub,      "vD, vA, vB"},
    {"vavguh",     Form::VX,  4, 1090, vavguh,      "vD, vA, vB"},
    {"vavguw",     Form::VX,  4, 1154, vavguw,      "vD, vA, vB"},
    {"vavgsb",     Form::VX,  4, 1282, vavgsb,      "vD, vA, vB"},
    {"vavgsh",     Form::VX,  4, 1346, vavgsh,      "vD, vA, vB"},
    {"vavgsw",     Form::VX,  4, 1410, vavgsw,      "vD, vA, vB"},
    {"vrlb",       Form::VX,  4, 4,    vrlb,        "vD, vA, vB"},
    {"vrlh",       Form::VX,  4, 68,   vrlh,        "vD, vA, vB"},
    {"vrlw",       Form::VX,  4, 132,  vrlw,        "vD, vA, vB"},
    {"vslb",       Form::VX,  4, 260,  vslb,        "vD, vA, vB"},
    {"vslh",       Form::VX,  4, 324,  vslh,        "vD, vA, vB"},
    {"vslw",       Form::VX,  4, 388,  vslw,        "vD, vA, vB"},
    {"vsrb",       Form::VX,  4, 516,  vsrb,        "vD, vA, vB"},
    {"vsrh",       Form::VX,  4, 580,  vsrh,        "vD, vA, vB"},
    {"vsrw",       Form::VX,  4, 644,  vsrw,        "vD, vA, vB"},
    {"vsrab",      Form::VX,  4, 772,  vsrab,       "vD, vA, vB"},
    {"vsrah",      Form::VX,  4, 836,  vsrah,       "vD, vA, vB"},
    {"vsraw",      Form::VX,  4, 900,  vsraw,       "vD, vA, vB"},
    {"vand",       Form::VX,  4, 1028, vand,        "vD, vA, vB"},
    {"vandc",      Form::VX,  4, 1092, vandc,       "vD, vA, vB"},
    {"vor",        Form::VX,  4, 1156, vor,         "vD, vA, vB"},
    {"vnor",       Form::VX,  4, 1284, vnor,        "vD, vA, vB"},
    {"vxor",       Form::VX,  4, 1220, vxor,        "vD, vA, vB"},
    {"vmuleub",    Form::VX,  4, 520,  vmuleub,     "vD, vA, vB"},
    {"vmuleuh",    Form::VX,  4, 584,  vmuleuh,     "vD, vA, vB"},
    {"vmulesb",    Form::VX,  4, 776,  vmulesb,     "vD, vA, vB"},
    {"vmulesh",    Form::VX,  4, 840,  vmulesh,     "vD, vA, vB"},
    {"vmuloub",    Form::VX,  4, 8,    vmuloub,     "vD, vA, vB"},
    {"vmulouh",    Form::VX,  4, 72,   vmulouh,     "vD, vA, vB"},
    {"vmulosb",    Form::VX,  4, 264,  vmulosb,     "vD, vA, vB"},
    {"vmulosh",    Form::VX,  4, 328,  vmulosh,     "vD, vA, vB"},
    {"vmhaddshs",  Form::VA,  4, 32,   vmhaddshs,   "vD, vA, vB, vC"},
    {"vmhraddshs", Form::VA,  4, 33,   vmhraddshs,  "vD, vA, vB, vC"},
    {"vmladduhm",  Form::VA,  4, 34,   vmladduhm,   "vD, vA, vB, vC"},
    {"vmsumubm",   Form::VA,  4, 36,   vmsumubm,    "vD, vA, vB, vC"},
    {"vmsummbm",   Form::VA,  4, 37,   vmsummbm,    "vD, vA, vB, vC"},
    {"vmsumuhm",   Form::VA,  4, 38,   vmsumuhm,    "vD, vA, vB, vC"},
    {"vmsumuhs",   Form::VA,  4, 39,   vmsumuhs,    "vD, vA, vB, vC"},
    {"vmsumshm",   Form::VA,  4, 40,   vmsumshm,    "vD, vA, vB, vC"},
    {"vmsumshs",   Form::VA,  4, 41,   vmsumshs,    "vD, vA, vB, vC"},
    {"vsum4ubs",   Form::VX,  4, 1544, vsum4ubs,    "vD, vA, vB"},
    {"vsum4sbs",   Form::VX,  4, 1800, vsum4sbs,    "vD, vA, vB"},
    {"vsum4shs",   Form::VX,  4, 1608, vsum4shs,    "vD, vA, vB"},
    {"vsum2sws",   Form::VX,  4, 1672, vsum2sws,    "vD, vA, vB"},
    {"vsumsws",    Form::VX,  4, 1928, vsumsws,     "vD, vA, vB"},
    {"vcmpequb",   Form::VC,  4, 6,    vcmpequb,    "vD, vA, vB",         k_rc_vector},
    {"vcmpequh",   Form::VC,  4, 70,   vcmpequh,    "vD, vA, vB",         k_rc_vector},
    {"vcmpequw",   Form::VC,  4, 134,  vcmpequw,    "vD, vA, vB",         k_rc_vector},
    {"vcmpgtub",   Form::VC,  4, 518,  vcmpgtub,    "vD, vA, vB",         k_rc_vector},
    {"vcmpgtuh",   Form::VC,  4, 582,  vcmpgtuh,    "vD, vA, vB",         k_rc_vector},
    {"vcmpgtuw",   Form::VC,  4, 646,  vcmpgtuw,    "vD, vA, vB",         k_rc_vector},
    {"vcmpgtsb",   Form::VC,  4, 774,  vcmpgtsb,    "vD, vA, vB",         k_rc_vector},
    {"vcmpgtsh",   Form::VC,  4, 838,  vcmpgtsh,    "vD, vA, vB",         k_rc_vector},
    {"vcmpgtsw",   Form::VC,  4, 902,  vcmpgtsw,    "vD, vA, vB",         k_rc_vector},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> vector_integer_opcodes() {
    return k_opcodes;
}

} // namespace ppu
