// Floating-point instructions.
// Reference: SPU ISA v1.2, chapter 9 "Floating-Point Instructions".
//
// Single precision is the SPU's own extended-range format (spu_float). Double precision is
// IEEE 754, computed with soft_float, with these SPU details (section 9.2):
//  - Each doubleword slot has its own rounding mode, from the FPSCR.
//  - Exceptions never trap; they set sticky flags in the FPSCR.
//  - UNVERIFIED: the Cell's SPU treats denormal operands as zero (setting DENORM), and returns
//    the default NaN for any NaN operand (setting NAN). The manual allows either choice;
//    later chips propagate NaNs and handle denormals fully.
//
// dfceq, dfcmeq, dfcgt, dfcmgt and dftsv are left out: the manual marks them as optional
// instructions added in version 1.2 (2007), after the PS3's Cell. UNVERIFIED on hardware.

#include "common/float_bits.h"
#include "common/soft_float.h"
#include "cpu/spu/ops.h"
#include "cpu/spu/spu_float.h"

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace spu {

namespace {

using namespace ops;
namespace sf = soft_float;

constexpr u32 k_single_sign = 0x8000'0000;
constexpr u32 k_single_magnitude = 0x7fff'ffff;

// ---- Single precision ----

void record_single_flags(Thread& t, u32 slot, const spu_float::Flags& flags) {
    const u32 first = k_fpscr_single_flags[slot];
    if (flags.overflow) t.state.set_fpscr_bit(first + k_single_overflow);
    if (flags.underflow) t.state.set_fpscr_bit(first + k_single_underflow);
    if (flags.diff) t.state.set_fpscr_bit(first + k_single_diff);
    if (flags.divide_by_zero) t.state.set_fpscr_bit(k_fpscr_divide_by_zero + slot);
}

// Computes each word slot with `f(a, b, c, flags)` and records the flags it raises.
template <typename F>
Vector each_single(Thread& t, Instruction op, F f) {
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        spu_float::Flags flags;
        result.set_word(i, f(ra(t, op).word(i), rb(t, op).word(i), rc(t, op).word(i), flags));
        record_single_flags(t, i, flags);
    }
    return result;
}

void fa(Thread& t, Instruction op) {
    set_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32, spu_float::Flags& flags) {
               return spu_float::add(a, b, flags);
           }));
}
void fs(Thread& t, Instruction op) {
    set_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32, spu_float::Flags& flags) {
               return spu_float::add(a, b ^ k_single_sign, flags);
           }));
}
void fm(Thread& t, Instruction op) {
    set_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32, spu_float::Flags& flags) {
               return spu_float::multiply(a, b, flags);
           }));
}
// a × b + c
void fma(Thread& t, Instruction op) {
    set_rrr_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32 c, spu_float::Flags& flags) {
                   return spu_float::multiply_add(a, b, c, flags);
               }));
}
// c - a × b
void fnms(Thread& t, Instruction op) {
    set_rrr_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32 c, spu_float::Flags& flags) {
                   return spu_float::multiply_add(a ^ k_single_sign, b, c, flags);
               }));
}
// a × b - c
void fms(Thread& t, Instruction op) {
    set_rrr_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32 c, spu_float::Flags& flags) {
                   return spu_float::multiply_add(a, b, c ^ k_single_sign, flags);
               }));
}

void frest(Thread& t, Instruction op) {
    set_rt(t, op, each_single(t, op, [](u32 a, u32, u32, spu_float::Flags& flags) {
               return spu_float::reciprocal_estimate(a, flags);
           }));
}
void frsqest(Thread& t, Instruction op) {
    set_rt(t, op, each_single(t, op, [](u32 a, u32, u32, spu_float::Flags& flags) {
               return spu_float::reciprocal_square_root_estimate(a, flags);
           }));
}
void fi(Thread& t, Instruction op) {
    set_rt(t, op, each_single(t, op, [](u32 a, u32 b, u32, spu_float::Flags& flags) {
               return spu_float::interpolate(a, b, flags);
           }));
}

// ---- Conversions ----
// The scale is encoded as 155 - I8 (integer to float) or 173 - I8 (float to integer).

s32 to_float_scale(Instruction op) {
    return 155 - static_cast<s32>(op.i8());
}
s32 to_integer_scale(Instruction op) {
    return 173 - static_cast<s32>(op.i8());
}

void csflt(Thread& t, Instruction op) {
    const s32 scale = to_float_scale(op);
    set_rt(t, op, each_single(t, op, [scale](u32 a, u32, u32, spu_float::Flags& flags) {
               return spu_float::from_integer(static_cast<s32>(a), scale, flags);
           }));
}
void cuflt(Thread& t, Instruction op) {
    const s32 scale = to_float_scale(op);
    set_rt(t, op, each_single(t, op, [scale](u32 a, u32, u32, spu_float::Flags& flags) {
               return spu_float::from_integer(a, scale, flags);
           }));
}
void cflts(Thread& t, Instruction op) {
    const s32 scale = to_integer_scale(op);
    set_rt(t, op, each_word(ra(t, op), {}, [scale](u32 a, u32) {
               return static_cast<u32>(spu_float::to_integer(a, scale, 32));
           }));
}
// Negative values saturate to zero, large ones to 0xffffffff.
void cfltu(Thread& t, Instruction op) {
    const s32 scale = to_integer_scale(op);
    set_rt(t, op, each_word(ra(t, op), {}, [scale](u32 a, u32) {
               const s64 value = spu_float::to_integer(a, scale, 64);
               return static_cast<u32>(std::clamp<s64>(value, 0, 0xffff'ffff));
           }));
}

// ---- Single-precision compares ----

void fceq(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return mask32(spu_float::equal(a, b));
           }));
}
void fcmeq(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return mask32(spu_float::equal(a & k_single_magnitude, b & k_single_magnitude));
           }));
}
void fcgt(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return mask32(spu_float::greater(a, b));
           }));
}
void fcmgt(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), [](u32 a, u32 b) {
               return mask32(spu_float::greater(a & k_single_magnitude, b & k_single_magnitude));
           }));
}

// ---- Double precision ----

struct DoubleFlags {
    sf::Flags ieee; // overflow, underflow, inexact
    bool invalid = false;
    bool nan = false;
    bool denorm = false;
};

void record_double_flags(Thread& t, u32 slot, const DoubleFlags& flags) {
    const u32 first = k_fpscr_double_flags[slot];
    if (flags.ieee.overflow) t.state.set_fpscr_bit(first + k_double_overflow);
    if (flags.ieee.underflow) t.state.set_fpscr_bit(first + k_double_underflow);
    if (flags.ieee.inexact) t.state.set_fpscr_bit(first + k_double_inexact);
    if (flags.invalid) t.state.set_fpscr_bit(first + k_double_invalid);
    if (flags.nan) t.state.set_fpscr_bit(first + k_double_nan);
    if (flags.denorm) t.state.set_fpscr_bit(first + k_double_denorm);
}

// RN0 and RN1 use soft_float's order: nearest, toward zero, toward +infinity, toward -infinity.
sf::Environment double_environment(const Thread& t, u32 slot) {
    const u32 bit = slot == 0 ? k_fpscr_rounding_slot0 : k_fpscr_rounding_slot1;
    const u32 mode = (u32{t.state.fpscr_bit(bit)} << 1) | u32{t.state.fpscr_bit(bit + 1)};
    sf::Environment environment;
    environment.rounding = static_cast<sf::Rounding>(mode);
    return environment;
}

// Any NaN operand gives the default NaN. Signaling NaNs are also invalid operations.
std::optional<u64> nan_result(std::initializer_list<u64> operands, DoubleFlags& flags) {
    bool any_nan = false;
    for (const u64 x : operands) {
        if (sf::is_signaling_nan(x)) flags.invalid = true;
        if (sf::is_nan(x)) any_nan = true;
    }
    if (!any_nan) return std::nullopt;
    flags.nan = true;
    return sf::k_default_nan;
}

// Denormal operands become zero, keeping their sign.
u64 flush_denormal(u64 x, DoubleFlags& flags) {
    if (!sf::is_denormal(x, sf::k_double)) return x;
    flags.denorm = true;
    return x & sf::k_sign_bit;
}

u64 invalid(DoubleFlags& flags) {
    flags.invalid = true;
    return sf::k_default_nan;
}

u64 double_add(u64 a, u64 b, sf::Environment environment, DoubleFlags& flags) {
    if (const auto nan = nan_result({a, b}, flags)) return *nan;
    a = flush_denormal(a, flags);
    b = flush_denormal(b, flags);
    if (sf::is_infinity(a) && sf::is_infinity(b) && sf::is_negative(a) != sf::is_negative(b)) {
        return invalid(flags);
    }
    return sf::add(a, b, sf::k_double, environment, flags.ieee);
}

u64 double_multiply(u64 a, u64 b, sf::Environment environment, DoubleFlags& flags) {
    if (const auto nan = nan_result({a, b}, flags)) return *nan;
    a = flush_denormal(a, flags);
    b = flush_denormal(b, flags);
    if ((sf::is_infinity(a) && sf::is_zero(b)) || (sf::is_zero(a) && sf::is_infinity(b))) {
        return invalid(flags);
    }
    return sf::multiply(a, b, sf::k_double, environment, flags.ieee);
}

// a × b + c, rounded once.
u64 double_multiply_add(u64 a, u64 b, u64 c, sf::Environment environment, DoubleFlags& flags) {
    if (const auto nan = nan_result({a, b, c}, flags)) return *nan;
    a = flush_denormal(a, flags);
    b = flush_denormal(b, flags);
    c = flush_denormal(c, flags);
    const bool infinite_product = sf::is_infinity(a) || sf::is_infinity(b);
    if (infinite_product && (sf::is_zero(a) || sf::is_zero(b))) return invalid(flags);
    const bool product_negative = sf::is_negative(a) != sf::is_negative(b);
    if (infinite_product && sf::is_infinity(c) && sf::is_negative(c) != product_negative) {
        return invalid(flags);
    }
    return sf::multiply_add(a, b, c, sf::k_double, environment, flags.ieee);
}

// The negated forms negate any result that isn't a NaN.
u64 negate_unless_nan(u64 x) {
    return sf::is_nan(x) ? x : sf::negate(x);
}

// Computes each doubleword slot with `f(a, b, rt, environment, flags)`.
template <typename F>
void each_double(Thread& t, Instruction op, F f) {
    Vector result;
    for (u32 i = 0; i < 2; ++i) {
        DoubleFlags flags;
        const u64 a = ra(t, op).dword(i), b = rb(t, op).dword(i), target = rt(t, op).dword(i);
        result.set_dword(i, f(a, b, target, double_environment(t, i), flags));
        record_double_flags(t, i, flags);
    }
    set_rt(t, op, result);
}

void dfa(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64, sf::Environment e, DoubleFlags& flags) {
        return double_add(a, b, e, flags);
    });
}
void dfs(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64, sf::Environment e, DoubleFlags& flags) {
        if (const auto nan = nan_result({a, b}, flags)) return *nan;
        return double_add(a, sf::negate(b), e, flags);
    });
}
void dfm(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64, sf::Environment e, DoubleFlags& flags) {
        return double_multiply(a, b, e, flags);
    });
}
// RT = a × b + RT
void dfma(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64 c, sf::Environment e, DoubleFlags& flags) {
        return double_multiply_add(a, b, c, e, flags);
    });
}
// RT = a × b - RT
void dfms(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64 c, sf::Environment e, DoubleFlags& flags) {
        if (const auto nan = nan_result({a, b, c}, flags)) return *nan;
        return double_multiply_add(a, b, sf::negate(c), e, flags);
    });
}
// RT = -(a × b - RT)
void dfnms(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64 c, sf::Environment e, DoubleFlags& flags) {
        if (const auto nan = nan_result({a, b, c}, flags)) return *nan;
        return negate_unless_nan(double_multiply_add(a, b, sf::negate(c), e, flags));
    });
}
// RT = -(a × b + RT)
void dfnma(Thread& t, Instruction op) {
    each_double(t, op, [](u64 a, u64 b, u64 c, sf::Environment e, DoubleFlags& flags) {
        return negate_unless_nan(double_multiply_add(a, b, c, e, flags));
    });
}

// ---- Conversions between single and double precision ----
// These use IEEE single precision (with infinities, NaNs and denormal results), not the
// extended range. Section 9.2.1.

bool is_single_nan(u32 x) {
    return (x & k_single_magnitude) > 0x7f80'0000;
}
bool is_single_signaling_nan(u32 x) {
    return is_single_nan(x) && (x & 0x0040'0000) == 0;
}
bool is_single_denormal(u32 x) {
    return (x & 0x7f80'0000) == 0 && (x & 0x007f'ffff) != 0;
}

// Each doubleword is rounded to single precision in its left word; the right word is zero.
void frds(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 2; ++i) {
        DoubleFlags flags;
        u32 single;
        if (const auto nan = nan_result({ra(t, op).dword(i)}, flags)) {
            single = 0x7fc0'0000; // the default single-precision NaN
        } else {
            const u64 x = flush_denormal(ra(t, op).dword(i), flags);
            single = double_to_single_bits(
                sf::round_to_format(x, sf::k_single, double_environment(t, i), flags.ieee));
        }
        result.set_word(2 * i, single);
        record_double_flags(t, i, flags);
    }
    set_rt(t, op, result);
}

// The left word of each doubleword is extended to double precision. Exact, apart from NaNs and
// denormals.
void fesd(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 2; ++i) {
        DoubleFlags flags;
        const u32 single = ra(t, op).word(2 * i);
        u64 extended;
        if (is_single_nan(single)) {
            flags.invalid = is_single_signaling_nan(single);
            flags.nan = true;
            extended = sf::k_default_nan;
        } else if (is_single_denormal(single)) {
            flags.denorm = true;
            extended = u64{single & k_single_sign} << 32;
        } else {
            extended = single_to_double_bits(single);
        }
        result.set_dword(i, extended);
        record_double_flags(t, i, flags);
    }
    set_rt(t, op, result);
}

// ---- Status and control register ----

// The FPSCR bits that exist; the rest read as zero. Section 9.3.
Vector fpscr_defined_bits() {
    State bits;
    auto set_range = [&](u32 first, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            bits.set_fpscr_bit(first + i);
        }
    };
    set_range(k_fpscr_rounding_slot0, 4);
    for (const u32 first : k_fpscr_single_flags) {
        set_range(first, 3);
    }
    for (const u32 first : k_fpscr_double_flags) {
        set_range(first, 6);
    }
    set_range(k_fpscr_divide_by_zero, 4);
    return bits.fpscr;
}

Vector defined_fpscr_bits_of(const Vector& v) {
    static const Vector defined = fpscr_defined_bits();
    return each_word(v, defined, [](u32 a, u32 b) { return a & b; });
}

void fscrrd(Thread& t, Instruction op) {
    set_rt(t, op, defined_fpscr_bits_of(t.state.fpscr));
}
// UNVERIFIED: the manual leaves undefined bits undefined after a write; we keep them zero.
void fscrwr(Thread& t, Instruction op) {
    t.state.fpscr = defined_fpscr_bits_of(ra(t, op));
}

constexpr OpcodeInfo k_opcodes[] = {
    {"fa", Form::RR, 0x2c4, fa, "RT, RA, RB"},
    {"dfa", Form::RR, 0x2cc, dfa, "RT, RA, RB"},
    {"fs", Form::RR, 0x2c5, fs, "RT, RA, RB"},
    {"dfs", Form::RR, 0x2cd, dfs, "RT, RA, RB"},
    {"fm", Form::RR, 0x2c6, fm, "RT, RA, RB"},
    {"dfm", Form::RR, 0x2ce, dfm, "RT, RA, RB"},
    {"fma", Form::RRR, 0xe, fma, "RT, RA, RB, RC"},
    {"dfma", Form::RR, 0x35c, dfma, "RT, RA, RB"},
    {"fnms", Form::RRR, 0xd, fnms, "RT, RA, RB, RC"},
    {"dfnms", Form::RR, 0x35e, dfnms, "RT, RA, RB"},
    {"fms", Form::RRR, 0xf, fms, "RT, RA, RB, RC"},
    {"dfms", Form::RR, 0x35d, dfms, "RT, RA, RB"},
    {"dfnma", Form::RR, 0x35f, dfnma, "RT, RA, RB"},
    {"frest", Form::RR, 0x1b8, frest, "RT, RA"},
    {"frsqest", Form::RR, 0x1b9, frsqest, "RT, RA"},
    {"fi", Form::RR, 0x3d4, fi, "RT, RA, RB"},
    {"csflt", Form::RI8, 0x1da, csflt, "RT, RA, scale155"},
    {"cflts", Form::RI8, 0x1d8, cflts, "RT, RA, scale173"},
    {"cuflt", Form::RI8, 0x1db, cuflt, "RT, RA, scale155"},
    {"cfltu", Form::RI8, 0x1d9, cfltu, "RT, RA, scale173"},
    {"frds", Form::RR, 0x3b9, frds, "RT, RA"},
    {"fesd", Form::RR, 0x3b8, fesd, "RT, RA"},
    {"fceq", Form::RR, 0x3c2, fceq, "RT, RA, RB"},
    {"fcmeq", Form::RR, 0x3ca, fcmeq, "RT, RA, RB"},
    {"fcgt", Form::RR, 0x2c2, fcgt, "RT, RA, RB"},
    {"fcmgt", Form::RR, 0x2ca, fcmgt, "RT, RA, RB"},
    {"fscrwr", Form::RR, 0x3ba, fscrwr, "RA"},
    {"fscrrd", Form::RR, 0x398, fscrrd, "RT"},
};

} // namespace

std::span<const OpcodeInfo> floating_point_opcodes() {
    return k_opcodes;
}

} // namespace spu
