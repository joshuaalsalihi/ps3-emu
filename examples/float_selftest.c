// Prints the exact bit patterns of compiler-generated floating-point arithmetic: sums,
// products, quotients, square roots, fused multiply-adds, conversions and comparisons, in
// double and single precision. The expected output comes from running the same C natively.
//
// Inputs are read through `volatile` so the compiler can't precompute the answers.

#include "lv2.h"

typedef unsigned int u32;

static u64 bits(double value) {
    union {
        double d;
        u64 u;
    } pun = {value};
    return pun.u;
}

static u64 float_bits(float value) {
    union {
        float f;
        u32 u;
    } pun = {value};
    return pun.u;
}

static void show(const char* name, u64 value) {
    print(name);
    print(" ");
    print_hex(value);
    print("\n");
}

static volatile double inputs[] = {
    1.0, 3.0, 0.1, -2.5, 1e300, 1e-300, 2.2250738585072014e-308, 5e-324, 123456789.987654321,
};
static volatile float single_inputs[] = {1.0f, 3.0f, 0.1f, -2.5f, 3.4e38f, 1.2e-38f, 1e-45f};

enum { k_count = sizeof inputs / sizeof inputs[0] };
enum { k_single_count = sizeof single_inputs / sizeof single_inputs[0] };

int main(void) {
    for (int i = 0; i < k_count; i++) {
        for (int j = 0; j < k_count; j++) {
            const double a = inputs[i], b = inputs[j];
            show("add", bits(a + b));
            show("sub", bits(a - b));
            show("mul", bits(a * b));
            show("div", bits(a / b));
            show("fma", bits(__builtin_fma(a, b, a)));
            show("lt", a < b);
        }
        const double a = inputs[i];
        show("sqrt", bits(__builtin_sqrt(a < 0 ? -a : a)));
        show("to_float", float_bits((float)a));
        show("to_int", (u64)(long long)(a > 9.2e18 || a < -9.2e18 ? 0 : a));
        show("from_int", bits((double)(long long)(a * 1000)));
    }
    for (int i = 0; i < k_single_count; i++) {
        for (int j = 0; j < k_single_count; j++) {
            const float a = single_inputs[i], b = single_inputs[j];
            show("adds", float_bits(a + b));
            show("muls", float_bits(a * b));
            show("divs", float_bits(a / b));
            show("fmas", float_bits(__builtin_fmaf(a, b, a)));
        }
    }
    print("done\n");
    return 0;
}
