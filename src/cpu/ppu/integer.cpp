// Fixed-point arithmetic, logical, compare, rotate and shift instructions.
// Reference: PowerPC Architecture Book I (v2.02), chapter 3, "Fixed-Point Processor".
// The PS3 runs games in 64-bit mode, so results, carries and CR0 use all 64 bits.

#include "cpu/ppu/opcodes.h"

#include <bit>
#include <limits>

namespace ppu {

namespace {

// CR0 reflects the sign of a result, plus a copy of XER[SO].
void update_cr0(Thread& t, u64 result) {
    const s64 value = static_cast<s64>(result);
    u32 field = value < 0 ? k_cr_lt : value > 0 ? k_cr_gt : k_cr_eq;
    if (t.state.xer.so) field |= k_cr_so;
    t.state.set_cr_field(0, field);
}

void set_overflow(Thread& t, bool overflow) {
    t.state.xer.ov = overflow;
    if (overflow) t.state.xer.so = true;
}

// Writes rD, then the optional XER[OV] and CR0 updates of an XO-form instruction.
void finish_xo(Thread& t, Instruction op, u64 result, bool overflow) {
    t.gpr(op.rd()) = result;
    if (op.oe()) set_overflow(t, overflow);
    if (op.rc()) update_cr0(t, result);
}

// Writes rA, then the optional CR0 update of a logical, rotate or shift instruction.
void finish_ra(Thread& t, Instruction op, u64 result) {
    t.gpr(op.ra()) = result;
    if (op.rc()) update_cr0(t, result);
}

// UNVERIFIED: the manuals leave the high word of 32-bit multiply/divide results undefined.
// We clear it; real hardware may differ.
u64 with_undefined_high_word(u32 low) {
    return low;
}

// a + b + carry_in: the core of every add and subtract-from instruction. The manuals define
// subtraction as ~a + b + 1, and so do we.
struct Sum {
    u64 value;
    bool carry;
    bool overflow;
};

Sum add_with_carry(u64 a, u64 b, bool carry_in) {
    const u64 partial = a + b;
    const u64 value = partial + carry_in;
    const bool carry = partial < a || value < partial;
    // Signed overflow: the operands share a sign that the result doesn't.
    const bool overflow = ((~(a ^ b) & (a ^ value)) >> 63) != 0;
    return {value, carry, overflow};
}

// ---- Add and subtract ----

void addi(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.gpr_or_zero(op.ra()) + op.simm();
}

void addis(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.gpr_or_zero(op.ra()) + (static_cast<u64>(op.simm()) << 16);
}

void addic(Thread& t, Instruction op) {
    const Sum sum = add_with_carry(t.gpr(op.ra()), op.simm(), false);
    t.gpr(op.rd()) = sum.value;
    t.state.xer.ca = sum.carry;
}

void addic_record(Thread& t, Instruction op) {
    addic(t, op);
    update_cr0(t, t.gpr(op.rd()));
}

void subfic(Thread& t, Instruction op) {
    const Sum sum = add_with_carry(~t.gpr(op.ra()), op.simm(), true);
    t.gpr(op.rd()) = sum.value;
    t.state.xer.ca = sum.carry;
}

void add(Thread& t, Instruction op) {
    const Sum sum = add_with_carry(t.gpr(op.ra()), t.gpr(op.rb()), false);
    finish_xo(t, op, sum.value, sum.overflow);
}

void subf(Thread& t, Instruction op) {
    const Sum sum = add_with_carry(~t.gpr(op.ra()), t.gpr(op.rb()), true);
    finish_xo(t, op, sum.value, sum.overflow);
}

void neg(Thread& t, Instruction op) {
    const Sum sum = add_with_carry(~t.gpr(op.ra()), 0, true);
    finish_xo(t, op, sum.value, sum.overflow);
}

// The carrying forms: same as above, but they also set XER[CA].
void finish_carrying(Thread& t, Instruction op, Sum sum) {
    t.state.xer.ca = sum.carry;
    finish_xo(t, op, sum.value, sum.overflow);
}

void addc(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(t.gpr(op.ra()), t.gpr(op.rb()), false));
}

void adde(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(t.gpr(op.ra()), t.gpr(op.rb()), t.state.xer.ca));
}

void addme(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(t.gpr(op.ra()), ~u64{0}, t.state.xer.ca));
}

void addze(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(t.gpr(op.ra()), 0, t.state.xer.ca));
}

void subfc(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(~t.gpr(op.ra()), t.gpr(op.rb()), true));
}

void subfe(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(~t.gpr(op.ra()), t.gpr(op.rb()), t.state.xer.ca));
}

void subfme(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(~t.gpr(op.ra()), ~u64{0}, t.state.xer.ca));
}

void subfze(Thread& t, Instruction op) {
    finish_carrying(t, op, add_with_carry(~t.gpr(op.ra()), 0, t.state.xer.ca));
}

// ---- Multiply and divide ----

void mulli(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.gpr(op.ra()) * static_cast<u64>(op.simm());
}

void mullw(Thread& t, Instruction op) {
    const s64 product = s64{static_cast<s32>(t.gpr(op.ra()))} * static_cast<s32>(t.gpr(op.rb()));
    const bool overflow = product != static_cast<s32>(product);
    finish_xo(t, op, static_cast<u64>(product), overflow);
}

void mulhw(Thread& t, Instruction op) {
    const s64 product = s64{static_cast<s32>(t.gpr(op.ra()))} * static_cast<s32>(t.gpr(op.rb()));
    finish_xo(t, op, with_undefined_high_word(static_cast<u32>(product >> 32)), false);
}

void mulhwu(Thread& t, Instruction op) {
    const u64 product = u64{static_cast<u32>(t.gpr(op.ra()))} * static_cast<u32>(t.gpr(op.rb()));
    finish_xo(t, op, with_undefined_high_word(static_cast<u32>(product >> 32)), false);
}

void mulld(Thread& t, Instruction op) {
    const s64 a = static_cast<s64>(t.gpr(op.ra()));
    const s64 b = static_cast<s64>(t.gpr(op.rb()));
    const u64 low = static_cast<u64>(a) * static_cast<u64>(b);
    // The product fits in 64 bits iff the high half is just the sign extension of the low half.
    const bool overflow = mul_high_signed(a, b) != (static_cast<s64>(low) >> 63);
    finish_xo(t, op, low, overflow);
}

void mulhd(Thread& t, Instruction op) {
    const s64 high =
        mul_high_signed(static_cast<s64>(t.gpr(op.ra())), static_cast<s64>(t.gpr(op.rb())));
    finish_xo(t, op, static_cast<u64>(high), false);
}

void mulhdu(Thread& t, Instruction op) {
    finish_xo(t, op, mul_high_unsigned(t.gpr(op.ra()), t.gpr(op.rb())), false);
}

// UNVERIFIED: division by zero and MIN / -1 leave rD undefined. We return 0.
void divw(Thread& t, Instruction op) {
    const s32 dividend = static_cast<s32>(t.gpr(op.ra()));
    const s32 divisor = static_cast<s32>(t.gpr(op.rb()));
    const bool overflow =
        divisor == 0 || (dividend == std::numeric_limits<s32>::min() && divisor == -1);
    const s32 quotient = overflow ? 0 : dividend / divisor;
    finish_xo(t, op, with_undefined_high_word(static_cast<u32>(quotient)), overflow);
}

void divwu(Thread& t, Instruction op) {
    const u32 dividend = static_cast<u32>(t.gpr(op.ra()));
    const u32 divisor = static_cast<u32>(t.gpr(op.rb()));
    const bool overflow = divisor == 0;
    const u32 quotient = overflow ? 0 : dividend / divisor;
    finish_xo(t, op, with_undefined_high_word(quotient), overflow);
}

void divd(Thread& t, Instruction op) {
    const s64 dividend = static_cast<s64>(t.gpr(op.ra()));
    const s64 divisor = static_cast<s64>(t.gpr(op.rb()));
    const bool overflow =
        divisor == 0 || (dividend == std::numeric_limits<s64>::min() && divisor == -1);
    const s64 quotient = overflow ? 0 : dividend / divisor;
    finish_xo(t, op, static_cast<u64>(quotient), overflow);
}

void divdu(Thread& t, Instruction op) {
    const u64 dividend = t.gpr(op.ra());
    const u64 divisor = t.gpr(op.rb());
    const bool overflow = divisor == 0;
    finish_xo(t, op, overflow ? 0 : dividend / divisor, overflow);
}

// ---- Compare ----

template <typename T>
u32 compare(T a, T b) {
    return a < b ? k_cr_lt : a > b ? k_cr_gt : k_cr_eq;
}

// L = 0 compares the low words; L = 1 compares whole doublewords.
void set_compare_signed(Thread& t, Instruction op, u64 a, u64 b) {
    const u32 result = op.l() ? compare<s64>(a, b) : compare<s32>(a, b);
    t.state.set_cr_field(op.crfd(), result | (t.state.xer.so ? k_cr_so : 0));
}

void set_compare_unsigned(Thread& t, Instruction op, u64 a, u64 b) {
    const u32 result = op.l() ? compare<u64>(a, b) : compare<u32>(a, b);
    t.state.set_cr_field(op.crfd(), result | (t.state.xer.so ? k_cr_so : 0));
}

void cmp(Thread& t, Instruction op) {
    set_compare_signed(t, op, t.gpr(op.ra()), t.gpr(op.rb()));
}

void cmpi(Thread& t, Instruction op) {
    set_compare_signed(t, op, t.gpr(op.ra()), op.simm());
}

void cmpl(Thread& t, Instruction op) {
    set_compare_unsigned(t, op, t.gpr(op.ra()), t.gpr(op.rb()));
}

void cmpli(Thread& t, Instruction op) {
    set_compare_unsigned(t, op, t.gpr(op.ra()), op.uimm());
}

// ---- Logical ----

void andi_record(Thread& t, Instruction op) {
    t.gpr(op.ra()) = t.gpr(op.rs()) & op.uimm();
    update_cr0(t, t.gpr(op.ra()));
}

void andis_record(Thread& t, Instruction op) {
    t.gpr(op.ra()) = t.gpr(op.rs()) & (op.uimm() << 16);
    update_cr0(t, t.gpr(op.ra()));
}

void ori(Thread& t, Instruction op) {
    t.gpr(op.ra()) = t.gpr(op.rs()) | op.uimm();
}

void oris(Thread& t, Instruction op) {
    t.gpr(op.ra()) = t.gpr(op.rs()) | (op.uimm() << 16);
}

void xori(Thread& t, Instruction op) {
    t.gpr(op.ra()) = t.gpr(op.rs()) ^ op.uimm();
}

void xoris(Thread& t, Instruction op) {
    t.gpr(op.ra()) = t.gpr(op.rs()) ^ (op.uimm() << 16);
}

void and_(Thread& t, Instruction op) {
    finish_ra(t, op, t.gpr(op.rs()) & t.gpr(op.rb()));
}

void andc(Thread& t, Instruction op) {
    finish_ra(t, op, t.gpr(op.rs()) & ~t.gpr(op.rb()));
}

void or_(Thread& t, Instruction op) {
    finish_ra(t, op, t.gpr(op.rs()) | t.gpr(op.rb()));
}

void orc(Thread& t, Instruction op) {
    finish_ra(t, op, t.gpr(op.rs()) | ~t.gpr(op.rb()));
}

void xor_(Thread& t, Instruction op) {
    finish_ra(t, op, t.gpr(op.rs()) ^ t.gpr(op.rb()));
}

void nand(Thread& t, Instruction op) {
    finish_ra(t, op, ~(t.gpr(op.rs()) & t.gpr(op.rb())));
}

void nor(Thread& t, Instruction op) {
    finish_ra(t, op, ~(t.gpr(op.rs()) | t.gpr(op.rb())));
}

void eqv(Thread& t, Instruction op) {
    finish_ra(t, op, ~(t.gpr(op.rs()) ^ t.gpr(op.rb())));
}

void extsb(Thread& t, Instruction op) {
    finish_ra(t, op, sign_extend(t.gpr(op.rs()), 8));
}

void extsh(Thread& t, Instruction op) {
    finish_ra(t, op, sign_extend(t.gpr(op.rs()), 16));
}

void extsw(Thread& t, Instruction op) {
    finish_ra(t, op, sign_extend(t.gpr(op.rs()), 32));
}

void cntlzw(Thread& t, Instruction op) {
    finish_ra(t, op, std::countl_zero(static_cast<u32>(t.gpr(op.rs()))));
}

void cntlzd(Thread& t, Instruction op) {
    finish_ra(t, op, std::countl_zero(t.gpr(op.rs())));
}

// ---- Rotate ----

void rlwinm(Thread& t, Instruction op) {
    const u64 rotated = rotl32(t.gpr(op.rs()), op.sh());
    finish_ra(t, op, rotated & mask64(op.mb() + 32, op.me() + 32));
}

void rlwnm(Thread& t, Instruction op) {
    const u64 rotated = rotl32(t.gpr(op.rs()), t.gpr(op.rb()) & 0x1f);
    finish_ra(t, op, rotated & mask64(op.mb() + 32, op.me() + 32));
}

void rlwimi(Thread& t, Instruction op) {
    const u64 rotated = rotl32(t.gpr(op.rs()), op.sh());
    const u64 mask = mask64(op.mb() + 32, op.me() + 32);
    finish_ra(t, op, (rotated & mask) | (t.gpr(op.ra()) & ~mask));
}

void rldicl(Thread& t, Instruction op) {
    finish_ra(t, op, rotl64(t.gpr(op.rs()), op.sh64()) & mask64(op.mb64(), 63));
}

void rldicr(Thread& t, Instruction op) {
    finish_ra(t, op, rotl64(t.gpr(op.rs()), op.sh64()) & mask64(0, op.me64()));
}

void rldic(Thread& t, Instruction op) {
    finish_ra(t, op, rotl64(t.gpr(op.rs()), op.sh64()) & mask64(op.mb64(), 63 - op.sh64()));
}

void rldimi(Thread& t, Instruction op) {
    const u64 rotated = rotl64(t.gpr(op.rs()), op.sh64());
    const u64 mask = mask64(op.mb64(), 63 - op.sh64());
    finish_ra(t, op, (rotated & mask) | (t.gpr(op.ra()) & ~mask));
}

void rldcl(Thread& t, Instruction op) {
    const u32 amount = t.gpr(op.rb()) & 0x3f;
    finish_ra(t, op, rotl64(t.gpr(op.rs()), amount) & mask64(op.mb64(), 63));
}

void rldcr(Thread& t, Instruction op) {
    const u32 amount = t.gpr(op.rb()) & 0x3f;
    finish_ra(t, op, rotl64(t.gpr(op.rs()), amount) & mask64(0, op.me64()));
}

// ---- Shift ----
// Shift amounts include one extra bit: shifting by 32+ (words) or 64+ (doublewords) gives 0.

void slw(Thread& t, Instruction op) {
    const u32 n = t.gpr(op.rb()) & 0x3f;
    const u64 mask = n < 32 ? mask64(32, 63 - n) : 0;
    finish_ra(t, op, rotl32(t.gpr(op.rs()), n) & mask);
}

void srw(Thread& t, Instruction op) {
    const u32 n = t.gpr(op.rb()) & 0x3f;
    const u64 mask = n < 32 ? mask64(n + 32, 63) : 0;
    finish_ra(t, op, rotl32(t.gpr(op.rs()), 64 - n) & mask);
}

void sld(Thread& t, Instruction op) {
    const u32 n = t.gpr(op.rb()) & 0x7f;
    const u64 mask = n < 64 ? mask64(0, 63 - n) : 0;
    finish_ra(t, op, rotl64(t.gpr(op.rs()), n) & mask);
}

void srd(Thread& t, Instruction op) {
    const u32 n = t.gpr(op.rb()) & 0x7f;
    const u64 mask = n < 64 ? mask64(n, 63) : 0;
    finish_ra(t, op, rotl64(t.gpr(op.rs()), 64 - n) & mask);
}

// Algebraic (sign-preserving) right shifts. XER[CA] is set when a negative value loses
// one bits, so that sraw followed by addze rounds toward zero.
void shift_right_algebraic_word(Thread& t, Instruction op, u32 n) {
    const u64 value = t.gpr(op.rs());
    const u64 rotated = rotl32(value, 64 - n);
    const u64 mask = n < 32 ? mask64(n + 32, 63) : 0;
    const bool negative = (value >> 31) & 1;

    t.state.xer.ca = negative && static_cast<u32>(rotated & ~mask) != 0;
    finish_ra(t, op, (rotated & mask) | (negative ? ~mask : 0));
}

void shift_right_algebraic_doubleword(Thread& t, Instruction op, u32 n) {
    const u64 value = t.gpr(op.rs());
    const u64 rotated = rotl64(value, 64 - n);
    const u64 mask = n < 64 ? mask64(n, 63) : 0;
    const bool negative = (value >> 63) & 1;

    t.state.xer.ca = negative && (rotated & ~mask) != 0;
    finish_ra(t, op, (rotated & mask) | (negative ? ~mask : 0));
}

void sraw(Thread& t, Instruction op) {
    shift_right_algebraic_word(t, op, t.gpr(op.rb()) & 0x3f);
}

void srawi(Thread& t, Instruction op) {
    shift_right_algebraic_word(t, op, op.sh());
}

void srad(Thread& t, Instruction op) {
    shift_right_algebraic_doubleword(t, op, t.gpr(op.rb()) & 0x7f);
}

void sradi(Thread& t, Instruction op) {
    shift_right_algebraic_doubleword(t, op, op.sh64());
}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic  form        op  xo   handler       operands                   flags
    {"addi",     Form::D,    14, 0,   addi,         "rD, rA, SIMM"},
    {"addis",    Form::D,    15, 0,   addis,        "rD, rA, SIMM"},
    {"addic",    Form::D,    12, 0,   addic,        "rD, rA, SIMM"},
    {"addic.",   Form::D,    13, 0,   addic_record, "rD, rA, SIMM"},
    {"subfic",   Form::D,    8,  0,   subfic,       "rD, rA, SIMM"},
    {"mulli",    Form::D,    7,  0,   mulli,        "rD, rA, SIMM"},
    {"add",      Form::XO,   31, 266, add,          "rD, rA, rB",              k_rc | k_oe},
    {"addc",     Form::XO,   31, 10,  addc,         "rD, rA, rB",              k_rc | k_oe},
    {"adde",     Form::XO,   31, 138, adde,         "rD, rA, rB",              k_rc | k_oe},
    {"addme",    Form::XO,   31, 234, addme,        "rD, rA",                  k_rc | k_oe},
    {"addze",    Form::XO,   31, 202, addze,        "rD, rA",                  k_rc | k_oe},
    {"subf",     Form::XO,   31, 40,  subf,         "rD, rA, rB",              k_rc | k_oe},
    {"subfc",    Form::XO,   31, 8,   subfc,        "rD, rA, rB",              k_rc | k_oe},
    {"subfe",    Form::XO,   31, 136, subfe,        "rD, rA, rB",              k_rc | k_oe},
    {"subfme",   Form::XO,   31, 232, subfme,       "rD, rA",                  k_rc | k_oe},
    {"subfze",   Form::XO,   31, 200, subfze,       "rD, rA",                  k_rc | k_oe},
    {"neg",      Form::XO,   31, 104, neg,          "rD, rA",                  k_rc | k_oe},
    {"mullw",    Form::XO,   31, 235, mullw,        "rD, rA, rB",              k_rc | k_oe},
    {"mulhw",    Form::XO,   31, 75,  mulhw,        "rD, rA, rB",              k_rc},
    {"mulhwu",   Form::XO,   31, 11,  mulhwu,       "rD, rA, rB",              k_rc},
    {"mulld",    Form::XO,   31, 233, mulld,        "rD, rA, rB",              k_rc | k_oe},
    {"mulhd",    Form::XO,   31, 73,  mulhd,        "rD, rA, rB",              k_rc},
    {"mulhdu",   Form::XO,   31, 9,   mulhdu,       "rD, rA, rB",              k_rc},
    {"divw",     Form::XO,   31, 491, divw,         "rD, rA, rB",              k_rc | k_oe},
    {"divwu",    Form::XO,   31, 459, divwu,        "rD, rA, rB",              k_rc | k_oe},
    {"divd",     Form::XO,   31, 489, divd,         "rD, rA, rB",              k_rc | k_oe},
    {"divdu",    Form::XO,   31, 457, divdu,        "rD, rA, rB",              k_rc | k_oe},
    {"cmp",      Form::X,    31, 0,   cmp,          "crfD, L, rA, rB"},
    {"cmpl",     Form::X,    31, 32,  cmpl,         "crfD, L, rA, rB"},
    {"cmpi",     Form::D,    11, 0,   cmpi,         "crfD, L, rA, SIMM"},
    {"cmpli",    Form::D,    10, 0,   cmpli,        "crfD, L, rA, UIMM"},
    {"andi.",    Form::D,    28, 0,   andi_record,  "rA, rS, UIMM"},
    {"andis.",   Form::D,    29, 0,   andis_record, "rA, rS, UIMM"},
    {"ori",      Form::D,    24, 0,   ori,          "rA, rS, UIMM"},
    {"oris",     Form::D,    25, 0,   oris,         "rA, rS, UIMM"},
    {"xori",     Form::D,    26, 0,   xori,         "rA, rS, UIMM"},
    {"xoris",    Form::D,    27, 0,   xoris,        "rA, rS, UIMM"},
    {"and",      Form::X,    31, 28,  and_,         "rA, rS, rB",              k_rc},
    {"andc",     Form::X,    31, 60,  andc,         "rA, rS, rB",              k_rc},
    {"or",       Form::X,    31, 444, or_,          "rA, rS, rB",              k_rc},
    {"orc",      Form::X,    31, 412, orc,          "rA, rS, rB",              k_rc},
    {"xor",      Form::X,    31, 316, xor_,         "rA, rS, rB",              k_rc},
    {"nand",     Form::X,    31, 476, nand,         "rA, rS, rB",              k_rc},
    {"nor",      Form::X,    31, 124, nor,          "rA, rS, rB",              k_rc},
    {"eqv",      Form::X,    31, 284, eqv,          "rA, rS, rB",              k_rc},
    {"extsb",    Form::X,    31, 954, extsb,        "rA, rS",                  k_rc},
    {"extsh",    Form::X,    31, 922, extsh,        "rA, rS",                  k_rc},
    {"extsw",    Form::X,    31, 986, extsw,        "rA, rS",                  k_rc},
    {"cntlzw",   Form::X,    31, 26,  cntlzw,       "rA, rS",                  k_rc},
    {"cntlzd",   Form::X,    31, 58,  cntlzd,       "rA, rS",                  k_rc},
    {"rlwinm",   Form::M,    21, 0,   rlwinm,       "rA, rS, SH, MB, ME",      k_rc},
    {"rlwnm",    Form::M,    23, 0,   rlwnm,        "rA, rS, rB, MB, ME",      k_rc},
    {"rlwimi",   Form::M,    20, 0,   rlwimi,       "rA, rS, SH, MB, ME",      k_rc},
    {"rldicl",   Form::MD,   30, 0,   rldicl,       "rA, rS, sh, mb",          k_rc},
    {"rldicr",   Form::MD,   30, 1,   rldicr,       "rA, rS, sh, me",          k_rc},
    {"rldic",    Form::MD,   30, 2,   rldic,        "rA, rS, sh, mb",          k_rc},
    {"rldimi",   Form::MD,   30, 3,   rldimi,       "rA, rS, sh, mb",          k_rc},
    {"rldcl",    Form::MDS,  30, 8,   rldcl,        "rA, rS, rB, mb",          k_rc},
    {"rldcr",    Form::MDS,  30, 9,   rldcr,        "rA, rS, rB, me",          k_rc},
    {"slw",      Form::X,    31, 24,  slw,          "rA, rS, rB",              k_rc},
    {"srw",      Form::X,    31, 536, srw,          "rA, rS, rB",              k_rc},
    {"sld",      Form::X,    31, 27,  sld,          "rA, rS, rB",              k_rc},
    {"srd",      Form::X,    31, 539, srd,          "rA, rS, rB",              k_rc},
    {"sraw",     Form::X,    31, 792, sraw,         "rA, rS, rB",              k_rc},
    {"srawi",    Form::X,    31, 824, srawi,        "rA, rS, SH",              k_rc},
    {"srad",     Form::X,    31, 794, srad,         "rA, rS, rB",              k_rc},
    {"sradi",    Form::XS,   31, 413, sradi,        "rA, rS, sh",              k_rc},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> integer_opcodes() {
    return k_opcodes;
}

} // namespace ppu
