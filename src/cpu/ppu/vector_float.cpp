// Vector single-precision floating-point instructions.
// References: Power ISA v3.1, Book I, sections 6.6 and 6.10; Cell BE Programming Handbook v1.11,
// section A.3.3, "Graphics Rounding Mode".
//
// The Cell's vector unit computes in one of two modes:
//  - IEEE mode: IEEE single precision, rounded to nearest, with no status flags. If VSCR[NJ]
//    is set, denormal inputs and tiny results become zero (keeping their sign).
//  - Graphics rounding mode (Cell-specific): rounded toward zero, denormals are zero, and there
//    are no infinities or NaNs. The largest exponent is just a larger number, and results too
//    large saturate to the biggest value, 0x7fffffff.
// Either way the arithmetic is soft_float's; this file decodes and encodes the elements.

#include "common/float_bits.h"
#include "common/soft_float.h"
#include "common/soft_float_math.h"
#include "cpu/ppu/vector_ops.h"

#include <bit>

namespace ppu {

namespace {

using namespace vmx;
namespace sf = soft_float;

// ---- Element encoding ----

constexpr u32 k_single_sign = 0x8000'0000;
constexpr u32 k_single_quiet = 0x0040'0000;
constexpr u32 k_single_default_nan = 0x7fc0'0000;
constexpr u32 k_single_infinity = 0x7f80'0000;
constexpr u32 k_graphics_largest = 0x7fff'ffff;

// Graphics mode's exponent range runs one step further than IEEE single precision's, because
// exponent 255 is an ordinary number.
constexpr sf::Format k_graphics_format = {24, -126, 128, 192};

struct Mode {
    bool graphics;
    bool non_java; // flush denormals (always true in graphics mode)
};

Mode mode(const Thread& t) {
    return {t.vmx_graphics_mode, t.vmx_graphics_mode || (t.state.vscr & k_vscr_non_java) != 0};
}

bool is_single_nan(u32 x) {
    return (x & 0x7fff'ffff) > k_single_infinity;
}

bool is_single_denormal(u32 x) {
    return (x & 0x7f80'0000) == 0 && (x & 0x007f'ffff) != 0;
}

// An element as a double bit pattern for soft_float.
u64 decode(u32 x, Mode m) {
    const u64 sign = u64{x & k_single_sign} << 32;
    if (m.non_java && is_single_denormal(x)) return sign; // denormals are zero
    if (!m.graphics) return single_to_double_bits(x);

    // Graphics mode: every nonzero exponent, even 255, is a normal number.
    const u32 exponent = (x >> 23) & 0xff;
    if (exponent == 0) return sign;
    return sign | (u64{exponent - 127 + 1023} << 52) | (u64{x & 0x7f'ffff} << 29);
}

// The environment to round results in. In non-Java mode, underflow is "enabled" only so that
// soft_float reports every tiny result; encode() then replaces it with zero.
sf::Environment environment(Mode m) {
    sf::Environment environment;
    environment.rounding = m.graphics ? sf::Rounding::toward_zero : sf::Rounding::nearest_even;
    environment.underflow_enabled = m.non_java;
    return environment;
}

sf::Format format(Mode m) {
    return m.graphics ? k_graphics_format : sf::k_single;
}

// A soft_float result as an element.
u32 encode(u64 bits, const sf::Flags& flags, Mode m) {
    const u32 sign = sf::is_negative(bits) ? k_single_sign : 0;
    if (m.non_java && flags.underflow) return sign; // UNVERIFIED in graphics mode: the sign
    if (!m.graphics) return double_to_single_bits(bits);

    if (sf::is_zero(bits)) return sign;
    if (sf::is_infinity(bits)) return sign | k_graphics_largest;
    const u32 exponent = static_cast<u32>((bits >> 52) & 0x7ff) - 1023 + 127;
    return sign | (exponent << 23) | static_cast<u32>((bits >> 29) & 0x7f'ffff);
}

// IEEE mode's NaN rule: the first NaN of a, b, c (in that order) made quiet.
std::optional<u32> nan_result(Mode m, std::initializer_list<u32> operands) {
    if (m.graphics) return std::nullopt;
    for (const u32 x : operands) {
        if (is_single_nan(x)) return x | k_single_quiet;
    }
    return std::nullopt;
}

// Applies `f` to each word of a and b (and c). `f` gets decoded operands and returns a result
// or nothing (for an invalid operation, which gives the default NaN).
template <typename F>
void each_word(Thread& t, Instruction op, F f) {
    const Mode m = mode(t);
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        const u32 a = va(t, op).word(i), b = vb(t, op).word(i), c = vc(t, op).word(i);
        result.set_word(i, f(m, a, b, c));
    }
    set_vd(t, op, result);
}

// ---- Arithmetic ----

bool opposite_infinities(u64 a, u64 b) {
    return sf::is_infinity(a) && sf::is_infinity(b) && sf::is_negative(a) != sf::is_negative(b);
}

u32 add(Mode m, u32 a, u32 b) {
    if (const auto nan = nan_result(m, {a, b})) return *nan;
    const u64 x = decode(a, m), y = decode(b, m);
    if (opposite_infinities(x, y)) return k_single_default_nan;
    sf::Flags flags;
    return encode(sf::add(x, y, format(m), environment(m), flags), flags, m);
}

void vaddfp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32 a, u32 b, u32) { return add(m, a, b); });
}

void vsubfp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32 a, u32 b, u32) {
        if (const auto nan = nan_result(m, {a, b})) return *nan;
        return add(m, a, b ^ k_single_sign);
    });
}

// a × c + b, rounded once.
u32 multiply_add(Mode m, u32 a, u32 b, u32 c) {
    if (const auto nan = nan_result(m, {a, b, c})) return *nan;
    const u64 x = decode(a, m), y = decode(b, m), z = decode(c, m);
    const bool infinity_times_zero =
        (sf::is_infinity(x) && sf::is_zero(z)) || (sf::is_zero(x) && sf::is_infinity(z));
    const u64 product_sign = (x ^ z) & sf::k_sign_bit;
    const bool cancels = (sf::is_infinity(x) || sf::is_infinity(z)) && sf::is_infinity(y) &&
                         (y & sf::k_sign_bit) != product_sign;
    if (infinity_times_zero || cancels) return k_single_default_nan;

    sf::Flags flags;
    return encode(sf::multiply_add(x, z, y, format(m), environment(m), flags), flags, m);
}

void vmaddfp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32 a, u32 b, u32 c) { return multiply_add(m, a, b, c); });
}

// -(a × c - b): negated after rounding; NaNs keep their sign.
void vnmsubfp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32 a, u32 b, u32 c) {
        if (const auto nan = nan_result(m, {a, b, c})) return *nan;
        const u32 result = multiply_add(m, a, b ^ k_single_sign, c);
        return is_single_nan(result) ? result : result ^ k_single_sign;
    });
}

// ---- Maximum and minimum ----

double as_double(u64 bits) {
    return std::bit_cast<double>(bits);
}

// +0 counts as larger than -0.
u32 extreme(Mode m, u32 a, u32 b, bool maximum) {
    if (const auto nan = nan_result(m, {a, b})) return *nan;
    const u64 x = decode(a, m), y = decode(b, m);
    bool pick_x = maximum ? as_double(x) > as_double(y) : as_double(x) < as_double(y);
    if (as_double(x) == as_double(y)) pick_x = maximum ? !sf::is_negative(x) : sf::is_negative(x);
    return encode(pick_x ? x : y, {}, m);
}

void vmaxfp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32 a, u32 b, u32) { return extreme(m, a, b, true); });
}

void vminfp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32 a, u32 b, u32) { return extreme(m, a, b, false); });
}

// ---- Estimates ----
// UNVERIFIED: the hardware computes these from tables we don't have. We return values well
// within the documented accuracy, computed only with soft_float, so every host agrees.

constexpr u64 k_one = 0x3ff0'0000'0000'0000;

// Saturates an infinite estimate in graphics mode, which has no infinities.
u32 infinite(Mode m, bool negative) {
    const u32 sign = negative ? k_single_sign : 0;
    return sign | (m.graphics ? k_graphics_largest : k_single_infinity);
}

u32 reciprocal_estimate(Mode m, u32 b) {
    if (const auto nan = nan_result(m, {b})) return *nan;
    const u64 x = decode(b, m);
    if (sf::is_zero(x)) return infinite(m, sf::is_negative(x));
    sf::Flags flags;
    return encode(sf::divide(k_one, x, format(m), environment(m), flags), flags, m);
}

void vrefp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) { return reciprocal_estimate(m, b); });
}

// Graphics mode takes the absolute value first.
u32 reciprocal_square_root_estimate(Mode m, u32 b) {
    if (const auto nan = nan_result(m, {b})) return *nan;
    u64 x = decode(b, m);
    if (m.graphics) x &= ~sf::k_sign_bit;
    if (sf::is_zero(x)) return infinite(m, sf::is_negative(x));
    if (sf::is_negative(x)) return k_single_default_nan;

    sf::Flags flags;
    const u64 root = sf::square_root(x, sf::k_double, {}, flags);
    return encode(sf::divide(k_one, root, format(m), environment(m), flags), flags, m);
}

void vrsqrtefp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) { return reciprocal_square_root_estimate(m, b); });
}

u32 log2_estimate(Mode m, u32 b) {
    if (const auto nan = nan_result(m, {b})) return *nan;
    u64 x = decode(b, m);
    if (m.graphics) {
        x &= ~sf::k_sign_bit;
        if (sf::is_zero(x)) return 0xffff'ffff; // Programming Handbook, A.3.3.5
    }
    if (sf::is_zero(x)) return k_single_sign | k_single_infinity;
    if (sf::is_negative(x)) return k_single_default_nan;
    if (sf::is_infinity(x)) return k_single_infinity;

    sf::Flags flags;
    return encode(sf::round_to_format(sf::log2(x), format(m), environment(m), flags), flags, m);
}

void vlogefp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) { return log2_estimate(m, b); });
}

// 2^x: 2^floor(x) times 2^f for the fraction f, built from the factors 2^(2^-k) for each set
// bit k of f. Each factor is 2 square-rooted k times.
u32 power_of_two_estimate(Mode m, u32 b) {
    if (const auto nan = nan_result(m, {b})) return *nan;
    const u64 x = decode(b, m);
    sf::Flags flags;
    if (sf::is_infinity(x)) return sf::is_negative(x) ? 0 : k_single_infinity;
    if (!sf::is_negative(x) && as_double(x) >= 130.0) return infinite(m, false);
    if (sf::is_negative(x) && as_double(x) <= -160.0) return 0;

    const u64 whole = sf::round_to_integral(x, sf::Rounding::toward_negative);
    const u64 fraction = sf::add(x, sf::negate(whole), sf::k_double, {}, flags); // exact, in [0, 1)
    const s64 fraction_bits = sf::to_integer(sf::is_zero(fraction) ? 0 : fraction + (u64{24} << 52),
                                             64, sf::Rounding::toward_zero, flags)
                                  .value; // the top 24 bits of the fraction

    u64 result = k_one;
    u64 factor = 0x4000'0000'0000'0000; // 2.0
    for (int k = 1; k <= 24; ++k) {
        factor = sf::square_root(factor, sf::k_double, {}, flags); // 2^(2^-k)
        if (fraction_bits & (s64{1} << (24 - k))) {
            result = sf::multiply(result, factor, sf::k_double, {}, flags);
        }
    }

    // Scale by 2^floor(x), exactly representable as a double.
    const s64 exponent = sf::to_integer(whole, 64, sf::Rounding::toward_zero, flags).value;
    const u64 scale = static_cast<u64>(exponent + 1023) << 52;
    sf::Flags result_flags;
    return encode(sf::multiply(result, scale, format(m), environment(m), result_flags),
                  result_flags, m);
}

void vexptefp(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) { return power_of_two_estimate(m, b); });
}

// ---- Rounding to integer ----
// These behave as in IEEE mode even in graphics mode (Programming Handbook, A.3.3.9).

u32 round_to_integral(Mode m, u32 b, sf::Rounding rounding) {
    const Mode ieee = {false, m.non_java};
    if (const auto nan = nan_result(ieee, {b})) return *nan;
    return double_to_single_bits(sf::round_to_integral(decode(b, ieee), rounding));
}

void vrfin(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) {
        return round_to_integral(m, b, sf::Rounding::nearest_even);
    });
}

void vrfiz(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) {
        return round_to_integral(m, b, sf::Rounding::toward_zero);
    });
}

void vrfip(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) {
        return round_to_integral(m, b, sf::Rounding::toward_positive);
    });
}

void vrfim(Thread& t, Instruction op) {
    each_word(t, op, [](Mode m, u32, u32 b, u32) {
        return round_to_integral(m, b, sf::Rounding::toward_negative);
    });
}

// ---- Conversion ----

// vcfux, vcfsx: integer words to single precision (rounded to nearest), divided by 2^UIM.
void convert_from_integer(Thread& t, Instruction op, bool is_signed) {
    const u32 scale = op.vuimm();
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        const u32 word = vb(t, op).word(i);
        const s64 value = is_signed ? as_signed(word, Width::word) : s64{word};
        sf::Flags flags;
        const u64 converted = sf::from_integer(value, sf::k_single, {}, flags);
        const u64 scaled = sf::is_zero(converted) ? 0 : converted - (u64{scale} << 52); // exact
        result.set_word(i, double_to_single_bits(scaled));
    }
    set_vd(t, op, result);
}

void vcfux(Thread& t, Instruction op) {
    convert_from_integer(t, op, false);
}
void vcfsx(Thread& t, Instruction op) {
    convert_from_integer(t, op, true);
}

// vctuxs, vctsxs: single precision × 2^UIM to integer words, truncated and saturated (setting
// SAT). In IEEE mode a NaN gives 0 without setting SAT.
void convert_to_integer(Thread& t, Instruction op, bool is_signed) {
    const Mode m = mode(t);
    bool saturated = false;
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        const u32 word = vb(t, op).word(i);
        if (!m.graphics && is_single_nan(word)) continue; // 0
        u64 x = decode(word, m);
        if (!sf::is_zero(x) && !sf::is_infinity(x)) x += u64{op.vuimm()} << 52; // × 2^UIM

        sf::Flags flags;
        const s64 value = sf::to_integer(x, 64, sf::Rounding::toward_zero, flags).value;
        result.set_word(i, is_signed ? saturate_signed(value, Width::word, saturated)
                                     : saturate_unsigned(value, Width::word, saturated));
    }
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

void vctuxs(Thread& t, Instruction op) {
    convert_to_integer(t, op, false);
}
void vctsxs(Thread& t, Instruction op) {
    convert_to_integer(t, op, true);
}

// ---- Compare ----

// In IEEE mode, comparisons with a NaN are false. Graphics mode has no NaNs.
void compare(Thread& t, Instruction op, bool (*f)(double, double)) {
    const Mode m = mode(t);
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        const u32 a = va(t, op).word(i), b = vb(t, op).word(i);
        const bool nan = !m.graphics && (is_single_nan(a) || is_single_nan(b));
        const bool holds = !nan && f(as_double(decode(a, m)), as_double(decode(b, m)));
        result.set_word(i, holds ? ~u32{0} : 0);
    }
    set_vd(t, op, result);
    record_compare(t, op, result);
}

void vcmpeqfp(Thread& t, Instruction op) {
    compare(t, op, [](double a, double b) { return a == b; });
}
void vcmpgefp(Thread& t, Instruction op) {
    compare(t, op, [](double a, double b) { return a >= b; });
}
void vcmpgtfp(Thread& t, Instruction op) {
    compare(t, op, [](double a, double b) { return a > b; });
}

// vcmpbfp: is a within [-b, b]? Bit 0 of each result word is set if a > b, bit 1 if a < -b.
// The record form sets CR6 bit 2 if every element was in bounds.
void vcmpbfp(Thread& t, Instruction op) {
    const Mode m = mode(t);
    Vector result;
    bool all_in_bounds = true;
    for (u32 i = 0; i < 4; ++i) {
        const u32 a = va(t, op).word(i), b = vb(t, op).word(i);
        u32 word = 0xc000'0000; // what a NaN gives: out of both bounds
        if (m.graphics || (!is_single_nan(a) && !is_single_nan(b))) {
            const double x = as_double(decode(a, m)), y = as_double(decode(b, m));
            word = (x <= y ? 0 : 0x8000'0000) | (x >= -y ? 0 : 0x4000'0000);
        }
        result.set_word(i, word);
        all_in_bounds = all_in_bounds && word == 0;
    }
    set_vd(t, op, result);
    if (op.vrc()) t.state.set_cr_field(6, all_in_bounds ? 0b0010 : 0);
}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic   form      op xo    handler    operands          flags
    {"vaddfp",    Form::VX, 4, 10,   vaddfp,    "vD, vA, vB"},
    {"vsubfp",    Form::VX, 4, 74,   vsubfp,    "vD, vA, vB"},
    {"vmaddfp",   Form::VA, 4, 46,   vmaddfp,   "vD, vA, vC, vB"},
    {"vnmsubfp",  Form::VA, 4, 47,   vnmsubfp,  "vD, vA, vC, vB"},
    {"vmaxfp",    Form::VX, 4, 1034, vmaxfp,    "vD, vA, vB"},
    {"vminfp",    Form::VX, 4, 1098, vminfp,    "vD, vA, vB"},
    {"vrefp",     Form::VX, 4, 266,  vrefp,     "vD, vB"},
    {"vrsqrtefp", Form::VX, 4, 330,  vrsqrtefp, "vD, vB"},
    {"vlogefp",   Form::VX, 4, 458,  vlogefp,   "vD, vB"},
    {"vexptefp",  Form::VX, 4, 394,  vexptefp,  "vD, vB"},
    {"vrfin",     Form::VX, 4, 522,  vrfin,     "vD, vB"},
    {"vrfiz",     Form::VX, 4, 586,  vrfiz,     "vD, vB"},
    {"vrfip",     Form::VX, 4, 650,  vrfip,     "vD, vB"},
    {"vrfim",     Form::VX, 4, 714,  vrfim,     "vD, vB"},
    {"vcfux",     Form::VX, 4, 778,  vcfux,     "vD, vB, UIM"},
    {"vcfsx",     Form::VX, 4, 842,  vcfsx,     "vD, vB, UIM"},
    {"vctuxs",    Form::VX, 4, 906,  vctuxs,    "vD, vB, UIM"},
    {"vctsxs",    Form::VX, 4, 970,  vctsxs,    "vD, vB, UIM"},
    {"vcmpeqfp",  Form::VC, 4, 198,  vcmpeqfp,  "vD, vA, vB",     k_rc_vector},
    {"vcmpgefp",  Form::VC, 4, 454,  vcmpgefp,  "vD, vA, vB",     k_rc_vector},
    {"vcmpgtfp",  Form::VC, 4, 710,  vcmpgtfp,  "vD, vA, vB",     k_rc_vector},
    {"vcmpbfp",   Form::VC, 4, 966,  vcmpbfp,   "vD, vA, vB",     k_rc_vector},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> vector_float_opcodes() {
    return k_opcodes;
}

} // namespace ppu
