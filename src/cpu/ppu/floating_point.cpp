// Floating-point arithmetic, comparison, conversion and FPSCR instructions.
// Reference: Power ISA v3.1, Book I, chapter 4, "Floating-Point Facility". The arithmetic itself
// is in common/soft_float; this file adds PowerPC's NaN rules, exceptions and FPSCR updates.
//
// Single-precision instructions ("s" suffix) round to single precision but keep the result in
// double format. The manual leaves them undefined for operands that aren't representable in
// single precision; we compute the rounded result anyway.

#include "cpu/ppu/fpscr.h"
#include "cpu/ppu/opcodes.h"

#include <bit>
#include <initializer_list>
#include <optional>

namespace ppu {

namespace fpscr {

u32 result_class(u64 bits, soft_float::Format format) {
    using namespace soft_float;
    const bool negative = is_negative(bits);
    if (is_nan(bits)) return k_c | k_fu;
    if (is_infinity(bits)) return (negative ? k_fl : k_fg) | k_fu;
    if (is_zero(bits)) return k_fe | (negative ? k_c : 0);
    if (is_denormal(bits, format)) return k_c | (negative ? k_fl : k_fg);
    return negative ? k_fl : k_fg;
}

} // namespace fpscr

namespace {

using namespace fpscr;
using soft_float::Format;
using soft_float::k_double;
using soft_float::k_single;

// ---- Helpers ----

u64& fpr(Thread& t, u32 n) {
    return t.state.fpr[n];
}

void raise(Thread& t, u32 exceptions) {
    t.state.fpscr = with_exceptions(t.state.fpscr, exceptions);
}

void set_field(Thread& t, u32 mask, u32 value) {
    t.state.fpscr = (t.state.fpscr & ~mask) | (value & mask);
}

// The record forms (Rc = 1) copy FX, FEX, VX and OX into CR1.
void record(Thread& t, Instruction op) {
    if (op.rc()) t.state.set_cr_field(1, t.state.fpscr >> 28);
}

bool any_signaling_nan(std::initializer_list<u64> operands) {
    for (const u64 value : operands) {
        if (soft_float::is_signaling_nan(value)) return true;
    }
    return false;
}

// The first NaN among the operands, in the manual's priority order (FRA, FRB, FRC).
std::optional<u64> first_nan(std::initializer_list<u64> operands) {
    for (const u64 value : operands) {
        if (soft_float::is_nan(value)) return value;
    }
    return std::nullopt;
}

// Handles an instruction whose result is a NaN: either an operand is a NaN, or the operation is
// invalid (`invalid` holds the VX* bits). Returns false if neither applies.
//  - An enabled invalid operation (VE = 1) leaves the target and FPRF unchanged.
//  - Otherwise the result is the first NaN operand, made quiet, or the default NaN.
bool nan_result(Thread& t, Instruction op, u32 invalid, std::optional<u64> nan_operand) {
    if (invalid == 0 && !nan_operand) return false;

    if (invalid != 0) raise(t, invalid);
    set_field(t, k_fr | k_fi, 0);
    if (invalid == 0 || !(t.state.fpscr & k_ve)) {
        fpr(t, op.frd()) =
            nan_operand ? (*nan_operand | soft_float::k_quiet_bit) : soft_float::k_default_nan;
        set_field(t, k_fprf_mask, k_c | k_fu);
    }
    record(t, op);
    return true;
}

// Writes a rounded result, with FR, FI, FPRF and the exceptions rounding raised.
void finish(Thread& t, Instruction op, u64 result, const soft_float::Flags& flags, Format format) {
    u32 exceptions = 0;
    if (flags.overflow) exceptions |= k_ox;
    if (flags.underflow) exceptions |= k_ux;
    if (flags.inexact) exceptions |= k_xx;
    raise(t, exceptions);

    // UNVERIFIED: on a disabled overflow the manual leaves FR undefined; we report the rounding.
    set_field(t, k_fr, flags.fraction_rounded ? k_fr : 0);
    set_field(t, k_fi, flags.inexact ? k_fi : 0);
    set_field(t, k_fprf_mask, result_class(result, format));
    fpr(t, op.frd()) = result;
    record(t, op);
}

// A finite nonzero value divided by zero (or a reciprocal estimate of zero).
void zero_divide(Thread& t, Instruction op, bool negative) {
    raise(t, k_zx);
    set_field(t, k_fr | k_fi, 0);
    if (!(t.state.fpscr & k_ze)) {
        const u64 infinity =
            soft_float::k_positive_infinity | (negative ? soft_float::k_sign_bit : 0);
        fpr(t, op.frd()) = infinity;
        set_field(t, k_fprf_mask, result_class(infinity, k_double));
    }
    record(t, op);
}

bool opposite_infinities(u64 a, u64 b) {
    return soft_float::is_infinity(a) && soft_float::is_infinity(b) &&
           soft_float::is_negative(a) != soft_float::is_negative(b);
}

bool infinity_times_zero(u64 a, u64 b) {
    using namespace soft_float;
    return (is_infinity(a) && is_zero(b)) || (is_zero(a) && is_infinity(b));
}

// ---- Arithmetic ----

void add(Thread& t, Instruction op, Format format, bool subtract) {
    const u64 a = fpr(t, op.fra());
    const u64 b = fpr(t, op.frb());
    const u64 addend = subtract ? soft_float::negate(b) : b;

    const u32 invalid = any_signaling_nan({a, b})        ? k_vxsnan
                        : opposite_infinities(a, addend) ? k_vxisi
                                                         : 0;
    if (nan_result(t, op, invalid, first_nan({a, b}))) return;

    soft_float::Flags flags;
    const u64 sum = soft_float::add(a, addend, format, environment(t.state.fpscr), flags);
    finish(t, op, sum, flags, format);
}

void fadd(Thread& t, Instruction op) {
    add(t, op, k_double, false);
}
void fadds(Thread& t, Instruction op) {
    add(t, op, k_single, false);
}
void fsub(Thread& t, Instruction op) {
    add(t, op, k_double, true);
}
void fsubs(Thread& t, Instruction op) {
    add(t, op, k_single, true);
}

void multiply(Thread& t, Instruction op, Format format) {
    const u64 a = fpr(t, op.fra());
    const u64 c = fpr(t, op.frc());

    const u32 invalid = any_signaling_nan({a, c})   ? k_vxsnan
                        : infinity_times_zero(a, c) ? k_vximz
                                                    : 0;
    if (nan_result(t, op, invalid, first_nan({a, c}))) return;

    soft_float::Flags flags;
    const u64 product = soft_float::multiply(a, c, format, environment(t.state.fpscr), flags);
    finish(t, op, product, flags, format);
}

void fmul(Thread& t, Instruction op) {
    multiply(t, op, k_double);
}
void fmuls(Thread& t, Instruction op) {
    multiply(t, op, k_single);
}

void divide(Thread& t, Instruction op, Format format) {
    using namespace soft_float;
    const u64 a = fpr(t, op.fra());
    const u64 b = fpr(t, op.frb());

    const u32 invalid = any_signaling_nan({a, b})            ? k_vxsnan
                        : (is_infinity(a) && is_infinity(b)) ? k_vxidi
                        : (is_zero(a) && is_zero(b))         ? k_vxzdz
                                                             : 0;
    if (nan_result(t, op, invalid, first_nan({a, b}))) return;
    if (is_zero(b) && !is_infinity(a)) return zero_divide(t, op, is_negative(a) != is_negative(b));

    soft_float::Flags flags;
    const u64 quotient = soft_float::divide(a, b, format, environment(t.state.fpscr), flags);
    finish(t, op, quotient, flags, format);
}

void fdiv(Thread& t, Instruction op) {
    divide(t, op, k_double);
}
void fdivs(Thread& t, Instruction op) {
    divide(t, op, k_single);
}

bool negative_nonzero(u64 value) {
    return soft_float::is_negative(value) && !soft_float::is_zero(value) &&
           !soft_float::is_nan(value);
}

void square_root(Thread& t, Instruction op, Format format) {
    const u64 b = fpr(t, op.frb());
    const u32 invalid = any_signaling_nan({b}) ? k_vxsnan : negative_nonzero(b) ? k_vxsqrt : 0;
    if (nan_result(t, op, invalid, first_nan({b}))) return;

    soft_float::Flags flags;
    const u64 root = soft_float::square_root(b, format, environment(t.state.fpscr), flags);
    finish(t, op, root, flags, format);
}

void fsqrt(Thread& t, Instruction op) {
    square_root(t, op, k_double);
}
void fsqrts(Thread& t, Instruction op) {
    square_root(t, op, k_single);
}

// fmadd and friends: FRA × FRC ± FRB, rounded once, optionally negated afterwards.
// Invalid-operation checks follow the manual's bfp_MULTIPLY_ADD: a signaling NaN rules out the
// others; then infinity × zero; then an infinite product plus an opposite infinity.
void multiply_add(Thread& t, Instruction op, Format format, bool subtract, bool negate_result) {
    const u64 a = fpr(t, op.fra());
    const u64 b = fpr(t, op.frb());
    const u64 c = fpr(t, op.frc());
    const u64 addend = subtract ? soft_float::negate(b) : b;

    const bool product_infinite = soft_float::is_infinity(a) || soft_float::is_infinity(c);
    const u64 product_sign = (a ^ c) & soft_float::k_sign_bit;
    const bool cancels = product_infinite && !soft_float::is_nan(a) && !soft_float::is_nan(c) &&
                         soft_float::is_infinity(addend) &&
                         (addend & soft_float::k_sign_bit) != product_sign;

    const u32 invalid = any_signaling_nan({a, b, c}) ? k_vxsnan
                        : infinity_times_zero(a, c)  ? k_vximz
                        : cancels                    ? k_vxisi
                                                     : 0;
    if (nan_result(t, op, invalid, first_nan({a, b, c}))) return;

    soft_float::Flags flags;
    u64 result = soft_float::multiply_add(a, c, addend, format, environment(t.state.fpscr), flags);
    if (negate_result) result = soft_float::negate(result);
    finish(t, op, result, flags, format);
}

void fmadd(Thread& t, Instruction op) {
    multiply_add(t, op, k_double, false, false);
}
void fmadds(Thread& t, Instruction op) {
    multiply_add(t, op, k_single, false, false);
}
void fmsub(Thread& t, Instruction op) {
    multiply_add(t, op, k_double, true, false);
}
void fmsubs(Thread& t, Instruction op) {
    multiply_add(t, op, k_single, true, false);
}
void fnmadd(Thread& t, Instruction op) {
    multiply_add(t, op, k_double, false, true);
}
void fnmadds(Thread& t, Instruction op) {
    multiply_add(t, op, k_single, false, true);
}
void fnmsub(Thread& t, Instruction op) {
    multiply_add(t, op, k_double, true, true);
}
void fnmsubs(Thread& t, Instruction op) {
    multiply_add(t, op, k_single, true, true);
}

// ---- Estimates ----
// UNVERIFIED: the Cell's estimates come from hardware tables we don't have. We return the
// correctly rounded value, which is within the accuracy the manual requires. The manual leaves
// FR and FI undefined; we clear them. Estimates don't set XX.

void estimate_result(Thread& t, Instruction op, u64 result, const soft_float::Flags& flags,
                     Format format) {
    raise(t, (flags.overflow ? k_ox : 0) | (flags.underflow ? k_ux : 0));
    set_field(t, k_fr | k_fi, 0);
    set_field(t, k_fprf_mask, result_class(result, format));
    fpr(t, op.frd()) = result;
    record(t, op);
}

constexpr u64 k_one = 0x3ff0'0000'0000'0000;

// fres: reciprocal estimate, single precision.
void fres(Thread& t, Instruction op) {
    const u64 b = fpr(t, op.frb());
    if (nan_result(t, op, any_signaling_nan({b}) ? k_vxsnan : 0, first_nan({b}))) return;
    if (soft_float::is_zero(b)) return zero_divide(t, op, soft_float::is_negative(b));

    soft_float::Flags flags;
    const u64 result = soft_float::divide(k_one, b, k_single, {}, flags);
    estimate_result(t, op, result, flags, k_single);
}

// frsqrte: reciprocal square root estimate, double precision.
void frsqrte(Thread& t, Instruction op) {
    const u64 b = fpr(t, op.frb());
    const u32 invalid = any_signaling_nan({b}) ? k_vxsnan : negative_nonzero(b) ? k_vxsqrt : 0;
    if (nan_result(t, op, invalid, first_nan({b}))) return;
    if (soft_float::is_zero(b)) return zero_divide(t, op, soft_float::is_negative(b));

    soft_float::Flags flags;
    const u64 root = soft_float::square_root(b, k_double, {}, flags);
    const u64 result = soft_float::divide(k_one, root, k_double, {}, flags);
    estimate_result(t, op, result, flags, k_double);
}

// ---- Rounding and conversion ----

// frsp: round to single precision. A NaN keeps only the bits single precision can hold.
void frsp(Thread& t, Instruction op) {
    const u64 b = fpr(t, op.frb());
    const std::optional<u64> nan =
        soft_float::is_nan(b) ? std::optional(b & ~u64{0x1fff'ffff}) : std::nullopt;
    if (nan_result(t, op, any_signaling_nan({b}) ? k_vxsnan : 0, nan)) return;

    soft_float::Flags flags;
    const u64 result = soft_float::round_to_format(b, k_single, environment(t.state.fpscr), flags);
    finish(t, op, result, flags, k_single);
}

// fctiw, fctiwz, fctid, fctidz: convert to a 32- or 64-bit integer. NaNs and out-of-range values
// are invalid; disabled, they saturate (NaNs to the most negative integer). FPRF is undefined
// afterwards; we leave it unchanged.
void convert_to_integer(Thread& t, Instruction op, u32 bits, bool truncate) {
    const u64 b = fpr(t, op.frb());
    const soft_float::Rounding rounding =
        truncate ? soft_float::Rounding::toward_zero : environment(t.state.fpscr).rounding;

    soft_float::Flags flags;
    s64 value = 0;
    u32 invalid = 0;
    if (soft_float::is_nan(b)) {
        invalid = k_vxcvi | (soft_float::is_signaling_nan(b) ? k_vxsnan : 0);
        value = bits == 64 ? s64{-0x7fff'ffff'ffff'ffff} - 1 : s64{-0x7fff'ffff} - 1;
    } else {
        const soft_float::IntegerResult result = soft_float::to_integer(b, bits, rounding, flags);
        if (result.out_of_range) invalid = k_vxcvi;
        value = result.value;
    }

    // UNVERIFIED: the high word of a 32-bit result is undefined; we clear it.
    const u64 result = bits == 64 ? static_cast<u64>(value) : static_cast<u32>(value);

    if (invalid != 0) {
        raise(t, invalid);
        set_field(t, k_fr | k_fi, 0);
        if (!(t.state.fpscr & k_ve)) fpr(t, op.frd()) = result;
        record(t, op);
        return;
    }

    raise(t, flags.inexact ? k_xx : 0);
    set_field(t, k_fr, flags.fraction_rounded ? k_fr : 0);
    set_field(t, k_fi, flags.inexact ? k_fi : 0);
    fpr(t, op.frd()) = result;
    record(t, op);
}

void fctiw(Thread& t, Instruction op) {
    convert_to_integer(t, op, 32, false);
}
void fctiwz(Thread& t, Instruction op) {
    convert_to_integer(t, op, 32, true);
}
void fctid(Thread& t, Instruction op) {
    convert_to_integer(t, op, 64, false);
}
void fctidz(Thread& t, Instruction op) {
    convert_to_integer(t, op, 64, true);
}

// fcfid: convert a 64-bit integer (the bits of FRB) to double precision.
void fcfid(Thread& t, Instruction op) {
    soft_float::Flags flags;
    const s64 value = static_cast<s64>(fpr(t, op.frb()));
    const u64 result = soft_float::from_integer(value, k_double, environment(t.state.fpscr), flags);
    finish(t, op, result, flags, k_double);
}

// ---- Compare ----

// fcmpu and fcmpo set one bit of a CR field and of FPCC: less, greater, equal or unordered.
// Ordered comparisons also treat any NaN as invalid.
void compare(Thread& t, Instruction op, bool ordered) {
    const u64 a = fpr(t, op.fra());
    const u64 b = fpr(t, op.frb());

    u32 result = 0;
    if (soft_float::is_nan(a) || soft_float::is_nan(b)) {
        result = k_cr_so; // the unordered bit
        const bool signaling = any_signaling_nan({a, b});
        u32 invalid = signaling ? k_vxsnan : 0;
        if (ordered && (!signaling || !(t.state.fpscr & k_ve))) invalid |= k_vxvc;
        raise(t, invalid);
    } else {
        // Comparing non-NaN doubles is exact, so the host's comparison is safe to use.
        const double x = std::bit_cast<double>(a);
        const double y = std::bit_cast<double>(b);
        result = x < y ? k_cr_lt : x > y ? k_cr_gt : k_cr_eq;
    }
    set_field(t, k_fpcc_mask, result << 12);
    t.state.set_cr_field(op.crfd(), result);
}

void fcmpu(Thread& t, Instruction op) {
    compare(t, op, false);
}
void fcmpo(Thread& t, Instruction op) {
    compare(t, op, true);
}

// ---- Moves and select (no FPSCR changes) ----

void move(Thread& t, Instruction op, u64 value) {
    fpr(t, op.frd()) = value;
    record(t, op);
}

void fmr(Thread& t, Instruction op) {
    move(t, op, fpr(t, op.frb()));
}
void fneg(Thread& t, Instruction op) {
    move(t, op, fpr(t, op.frb()) ^ soft_float::k_sign_bit);
}
void fabs(Thread& t, Instruction op) {
    move(t, op, fpr(t, op.frb()) & ~soft_float::k_sign_bit);
}
void fnabs(Thread& t, Instruction op) {
    move(t, op, fpr(t, op.frb()) | soft_float::k_sign_bit);
}

// fsel: FRC if FRA ≥ 0 (including -0), otherwise FRB. A NaN in FRA selects FRB.
void fsel(Thread& t, Instruction op) {
    const u64 a = fpr(t, op.fra());
    const bool non_negative =
        !soft_float::is_nan(a) && (!soft_float::is_negative(a) || soft_float::is_zero(a));
    move(t, op, non_negative ? fpr(t, op.frc()) : fpr(t, op.frb()));
}

// ---- FPSCR ----

void mffs(Thread& t, Instruction op) {
    move(t, op, t.state.fpscr);
}

// mtfsf: copies the fields selected by FLM from the low word of FRB. FX is copied as given;
// FEX and VX are recomputed.
void mtfsf(Thread& t, Instruction op) {
    u32 mask = 0;
    for (u32 field = 0; field < 8; ++field) {
        if (op.flm() & (0x80 >> field)) mask |= 0xf0000000u >> (4 * field);
    }
    t.state.fpscr =
        with_summaries((t.state.fpscr & ~mask) | (static_cast<u32>(fpr(t, op.frb())) & mask));
    record(t, op);
}

// mtfsfi: sets one field to an immediate.
void mtfsfi(Thread& t, Instruction op) {
    const u32 shift = 28 - 4 * op.crfd();
    t.state.fpscr = with_summaries((t.state.fpscr & ~(0xfu << shift)) | (op.u() << shift));
    record(t, op);
}

// mtfsb0 and mtfsb1: clear or set one bit. FEX and VX can't be set or cleared this way, and
// setting an exception bit also sets FX.
void mtfsb0(Thread& t, Instruction op) {
    const u32 bit = 1u << (31 - op.crbd());
    if ((bit & (k_fex | k_vx)) == 0) t.state.fpscr = with_summaries(t.state.fpscr & ~bit);
    record(t, op);
}

void mtfsb1(Thread& t, Instruction op) {
    const u32 bit = 1u << (31 - op.crbd());
    if ((bit & (k_fex | k_vx)) == 0) t.state.fpscr = with_exceptions(t.state.fpscr, bit);
    record(t, op);
}

// mcrfs: copies an FPSCR field to a CR field, then clears the exception bits it copied (except
// FEX and VX, which are recomputed).
void mcrfs(Thread& t, Instruction op) {
    const u32 shift = 28 - 4 * op.crfs();
    const u32 field_mask = 0xfu << shift;
    t.state.set_cr_field(op.crfd(), t.state.fpscr >> shift);
    const u32 cleared = field_mask & (k_exception_bits | k_fx);
    t.state.fpscr = with_summaries(t.state.fpscr & ~cleared);
}

// ---- Store ----

// stfiwx: stores the low word of an FPR, unconverted.
void stfiwx(Thread& t, Instruction op) {
    const u64 ea = t.gpr_or_zero(op.ra()) + t.gpr(op.rb());
    t.memory.write(ea, static_cast<u32>(fpr(t, op.frs())));
}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic  form       op  xo   handler  operands                 flags
    {"fadd",     Form::A,   63, 21,  fadd,    "frD, frA, frB",         k_rc},
    {"fadds",    Form::A,   59, 21,  fadds,   "frD, frA, frB",         k_rc},
    {"fsub",     Form::A,   63, 20,  fsub,    "frD, frA, frB",         k_rc},
    {"fsubs",    Form::A,   59, 20,  fsubs,   "frD, frA, frB",         k_rc},
    {"fmul",     Form::A,   63, 25,  fmul,    "frD, frA, frC",         k_rc},
    {"fmuls",    Form::A,   59, 25,  fmuls,   "frD, frA, frC",         k_rc},
    {"fdiv",     Form::A,   63, 18,  fdiv,    "frD, frA, frB",         k_rc},
    {"fdivs",    Form::A,   59, 18,  fdivs,   "frD, frA, frB",         k_rc},
    {"fsqrt",    Form::A,   63, 22,  fsqrt,   "frD, frB",              k_rc},
    {"fsqrts",   Form::A,   59, 22,  fsqrts,  "frD, frB",              k_rc},
    {"fmadd",    Form::A,   63, 29,  fmadd,   "frD, frA, frC, frB",    k_rc},
    {"fmadds",   Form::A,   59, 29,  fmadds,  "frD, frA, frC, frB",    k_rc},
    {"fmsub",    Form::A,   63, 28,  fmsub,   "frD, frA, frC, frB",    k_rc},
    {"fmsubs",   Form::A,   59, 28,  fmsubs,  "frD, frA, frC, frB",    k_rc},
    {"fnmadd",   Form::A,   63, 31,  fnmadd,  "frD, frA, frC, frB",    k_rc},
    {"fnmadds",  Form::A,   59, 31,  fnmadds, "frD, frA, frC, frB",    k_rc},
    {"fnmsub",   Form::A,   63, 30,  fnmsub,  "frD, frA, frC, frB",    k_rc},
    {"fnmsubs",  Form::A,   59, 30,  fnmsubs, "frD, frA, frC, frB",    k_rc},
    {"fres",     Form::A,   59, 24,  fres,    "frD, frB",              k_rc},
    {"frsqrte",  Form::A,   63, 26,  frsqrte, "frD, frB",              k_rc},
    {"fsel",     Form::A,   63, 23,  fsel,    "frD, frA, frC, frB",    k_rc},
    {"frsp",     Form::X,   63, 12,  frsp,    "frD, frB",              k_rc},
    {"fctiw",    Form::X,   63, 14,  fctiw,   "frD, frB",              k_rc},
    {"fctiwz",   Form::X,   63, 15,  fctiwz,  "frD, frB",              k_rc},
    {"fctid",    Form::X,   63, 814, fctid,   "frD, frB",              k_rc},
    {"fctidz",   Form::X,   63, 815, fctidz,  "frD, frB",              k_rc},
    {"fcfid",    Form::X,   63, 846, fcfid,   "frD, frB",              k_rc},
    {"fcmpu",    Form::X,   63, 0,   fcmpu,   "crfD, frA, frB"},
    {"fcmpo",    Form::X,   63, 32,  fcmpo,   "crfD, frA, frB"},
    {"fmr",      Form::X,   63, 72,  fmr,     "frD, frB",              k_rc},
    {"fneg",     Form::X,   63, 40,  fneg,    "frD, frB",              k_rc},
    {"fabs",     Form::X,   63, 264, fabs,    "frD, frB",              k_rc},
    {"fnabs",    Form::X,   63, 136, fnabs,   "frD, frB",              k_rc},
    {"mffs",     Form::X,   63, 583, mffs,    "frD",                   k_rc},
    {"mtfsf",    Form::XFX, 63, 711, mtfsf,   "FLM, frB",              k_rc},
    {"mtfsfi",   Form::X,   63, 134, mtfsfi,  "crfD, U",               k_rc},
    {"mtfsb0",   Form::X,   63, 70,  mtfsb0,  "crbD",                  k_rc},
    {"mtfsb1",   Form::X,   63, 38,  mtfsb1,  "crbD",                  k_rc},
    {"mcrfs",    Form::X,   63, 64,  mcrfs,   "crfD, crfS"},
    {"stfiwx",   Form::X,   31, 983, stfiwx,  "frS, rA, rB"},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> floating_point_opcodes() {
    return k_opcodes;
}

} // namespace ppu
