#include "rsx/shader_float.h"

#include "common/float_bits.h"
#include "common/soft_float.h"

namespace rsx::shader_float {

namespace {

constexpr u32 k_sign = 0x8000'0000;

bool is_nan(Float a) {
    return (a & 0x7fff'ffff) > 0x7f80'0000;
}
bool is_infinity(Float a) {
    return (a & 0x7fff'ffff) == 0x7f80'0000;
}
bool is_zero(Float a) {
    return (a & 0x7fff'ffff) == 0;
}

u64 wide(Float a) {
    return single_to_double_bits(a);
}

// Runs a soft_float operation in single precision and narrows the result.
template <typename Operation>
Float single(Operation operation) {
    soft_float::Flags flags;
    return double_to_single_bits(operation(soft_float::k_single, soft_float::Environment{}, flags));
}

} // namespace

Float add(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return k_default_nan;
    if (is_infinity(a) && is_infinity(b) && ((a ^ b) & k_sign)) return k_default_nan;
    return single([&](auto format, auto environment, auto& flags) {
        return soft_float::add(wide(a), wide(b), format, environment, flags);
    });
}

Float multiply(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return k_default_nan;
    if ((is_infinity(a) && is_zero(b)) || (is_zero(a) && is_infinity(b))) return k_default_nan;
    return single([&](auto format, auto environment, auto& flags) {
        return soft_float::multiply(wide(a), wide(b), format, environment, flags);
    });
}

Float multiply_add(Float a, Float b, Float c) {
    return add(multiply(a, b), c);
}

Float divide(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return k_default_nan;
    if ((is_zero(a) && is_zero(b)) || (is_infinity(a) && is_infinity(b))) return k_default_nan;
    return single([&](auto format, auto environment, auto& flags) {
        return soft_float::divide(wide(a), wide(b), format, environment, flags);
    });
}

Float reciprocal(Float a) {
    return divide(k_one, a);
}

Float reciprocal_square_root(Float a) {
    if (is_nan(a) || ((a & k_sign) && !is_zero(a))) return k_default_nan;
    const Float root = single([&](auto format, auto environment, auto& flags) {
        return soft_float::square_root(wide(a), format, environment, flags);
    });
    return reciprocal(root);
}

Float minimum(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return k_default_nan;
    return less(b, a) ? b : a;
}

Float maximum(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return k_default_nan;
    return less(a, b) ? b : a;
}

// Ordering IEEE values by their bits: flip negative values so larger bit patterns are larger.
bool less(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return false;
    if (is_zero(a) && is_zero(b)) return false;
    const auto key = [](Float x) { return (x & k_sign) ? ~x : x | k_sign; };
    return key(a) < key(b);
}

bool equal(Float a, Float b) {
    if (is_nan(a) || is_nan(b)) return false;
    return a == b || (is_zero(a) && is_zero(b));
}

Float floor(Float a) {
    if (is_nan(a)) return k_default_nan;
    return double_to_single_bits(
        soft_float::round_to_integral(wide(a), soft_float::Rounding::toward_negative));
}

Float from_integer(s64 value) {
    return single([&](auto format, auto environment, auto& flags) {
        return soft_float::from_integer(value, format, environment, flags);
    });
}

s64 to_integer(Float a) {
    if (is_nan(a)) return 0;
    soft_float::Flags flags;
    return soft_float::to_integer(wide(a), 64, soft_float::Rounding::nearest_even, flags).value;
}

} // namespace rsx::shader_float
