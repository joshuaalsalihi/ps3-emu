#pragma once

// Vector operations both RSX shader types share. Semantics: the ARB vertex and fragment program
// specifications, whose instructions the RSX's match.

#include "rsx/shader_float.h"

#include <array>

namespace rsx::shader_ops {

using shader_float::Float;
using shader_float::Vec4;

constexpr u32 k_sign = 0x8000'0000;

// Applies a source's swizzle, absolute value and negation, in that order.
inline Vec4 modify(const Vec4& value, const std::array<u8, 4>& swizzle, bool absolute,
                   bool negate) {
    Vec4 result;
    for (int i = 0; i < 4; ++i) {
        Float component = value[swizzle[i]];
        if (absolute) component &= ~k_sign;
        if (negate) component ^= k_sign;
        result[i] = component;
    }
    return result;
}

inline Vec4 splat(Float value) {
    return {value, value, value, value};
}

template <typename Operation>
Vec4 each(const Vec4& a, const Vec4& b, Operation operation) {
    return {operation(a[0], b[0]), operation(a[1], b[1]), operation(a[2], b[2]),
            operation(a[3], b[3])};
}

// The first `count` components' products, summed left to right.
inline Float dot(const Vec4& a, const Vec4& b, int count) {
    Float sum = shader_float::multiply(a[0], b[0]);
    for (int i = 1; i < count; ++i) {
        sum = shader_float::multiply_add(a[i], b[i], sum);
    }
    return sum;
}

inline bool greater_or_equal(Float a, Float b) {
    return shader_float::less(b, a) || shader_float::equal(a, b);
}

// 1.0 where `condition` holds for a component pair, else 0.0 (SLT, SGE, ...).
template <typename Condition>
Vec4 set_if(const Vec4& a, const Vec4& b, Condition condition) {
    return each(a, b, [&](Float x, Float y) {
        return condition(x, y) ? shader_float::k_one : shader_float::k_zero;
    });
}

inline Float saturate(Float value) {
    using namespace shader_float;
    return minimum(maximum(value, k_zero), k_one);
}

inline Float fraction(Float value) {
    return shader_float::add(value, shader_float::floor(value) ^ k_sign);
}

} // namespace rsx::shader_ops
