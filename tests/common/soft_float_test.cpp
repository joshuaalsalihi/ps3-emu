// soft_float is checked against the host's IEEE 754 arithmetic, an independent implementation:
// many random operations in every rounding mode, comparing results bit for bit, along with the
// inexact, overflow and divide-by-zero flags. The fraction-rounded flag is checked against the
// host's truncated result.
//
// Hosts may detect "tiny" results after rounding instead of before, as PowerPC does, so the
// underflow flag is checked with hand-worked cases instead.
//
// This file is compiled with -frounding-math, so the compiler respects fesetround.

#include "common/soft_float.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cfenv>
#include <cmath>
#include <functional>
#include <random>

using namespace soft_float;

namespace {

constexpr int k_iterations = 20'000;

constexpr Rounding k_roundings[] = {Rounding::nearest_even, Rounding::toward_zero,
                                    Rounding::toward_positive, Rounding::toward_negative};

int host_rounding(Rounding rounding) {
    switch (rounding) {
    case Rounding::nearest_even: return FE_TONEAREST;
    case Rounding::toward_zero: return FE_TOWARDZERO;
    case Rounding::toward_positive: return FE_UPWARD;
    case Rounding::toward_negative: return FE_DOWNWARD;
    }
    return FE_TONEAREST;
}

struct HostResult {
    u64 bits;
    bool inexact;
    bool overflow;
    bool divide_by_zero;
};

// Runs `operation` on the host FPU in the given rounding mode, capturing its flags.
HostResult on_host(Rounding rounding, const std::function<double()>& operation) {
    std::fesetround(host_rounding(rounding));
    std::feclearexcept(FE_ALL_EXCEPT);
    const double result = operation();
    const HostResult host = {std::bit_cast<u64>(result), std::fetestexcept(FE_INEXACT) != 0,
                             std::fetestexcept(FE_OVERFLOW) != 0,
                             std::fetestexcept(FE_DIVBYZERO) != 0};
    std::fesetround(FE_TONEAREST);
    return host;
}

// Random doubles that exercise the interesting cases: any exponent, exponents near the
// overflow and underflow boundaries, denormals, zeros and infinities. Never NaN.
class RandomValues {
public:
    explicit RandomValues(u64 seed) : engine_(seed) {}

    u64 next_double() {
        const u64 sign = (engine_() & 1) ? k_sign_bit : 0;
        const u64 fraction = engine_() & ((u64{1} << 52) - 1);
        switch (engine_() % 10) {
        case 0: return sign | fraction;                                      // denormal
        case 1: return sign | (u64(1 + engine_() % 60) << 52) | fraction;    // near the bottom
        case 2: return sign | (u64(2000 + engine_() % 46) << 52) | fraction; // near the top
        case 3: return sign | (engine_() % 3 == 0 ? 0 : k_positive_infinity);
        case 4: return sign | (u64(1023) << 52) | (fraction & ~u64{0xfff}); // 1.x, few bits
        default: return sign | (u64(1 + engine_() % 2045) << 52) | fraction;
        }
    }

    // A double that is exactly representable in single precision.
    u64 next_single() {
        u32 bits = static_cast<u32>(engine_());
        if ((bits & 0x7f80'0000) == 0x7f80'0000) bits &= 0xff80'0000; // NaN → infinity
        return std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(bits)));
    }

    // A second operand near the first, so subtraction cancels. (Infinities stay as they are, so
    // this never makes a NaN.)
    u64 nearby(u64 bits) {
        if (is_infinity(bits)) return bits;
        return bits ^ (engine_() & 0xffff);
    }

private:
    std::mt19937_64 engine_;
};

double as_double(u64 bits) {
    return std::bit_cast<double>(bits);
}

float as_float(u64 bits) {
    return static_cast<float>(as_double(bits)); // exact for single-representable values
}

// Compares one soft_float result with the host's, including the flags.
void check(const char* name, u64 a, u64 b, u64 c, Rounding rounding, u64 ours, const Flags& flags,
           const HostResult& host, const HostResult& truncated) {
    INFO(name << " a=" << std::hex << a << " b=" << b << " c=" << c
              << " rounding=" << static_cast<int>(rounding));
    REQUIRE(ours == host.bits);
    REQUIRE(flags.inexact == host.inexact);
    REQUIRE(flags.overflow == host.overflow);
    REQUIRE(flags.divide_by_zero == host.divide_by_zero);
    // Rounding incremented the fraction exactly when the result's magnitude is larger than the
    // truncated result's. (Only magnitudes: zero sums can differ in sign.)
    if (!flags.overflow) {
        REQUIRE(flags.fraction_rounded == ((ours & ~k_sign_bit) != (truncated.bits & ~k_sign_bit)));
    }
}

// x rounded to an integer in the given direction, without relying on the FPU's rounding mode.
double integral_on_host(double x, Rounding rounding) {
    switch (rounding) {
    case Rounding::nearest_even: return std::nearbyint(x); // the default mode: ties to even
    case Rounding::toward_zero: return std::trunc(x);
    case Rounding::toward_positive: return std::ceil(x);
    case Rounding::toward_negative: return std::floor(x);
    }
    return x;
}

} // namespace

// ---- Double precision ----

TEST_CASE("soft_float double arithmetic matches the host") {
    RandomValues random(1);
    for (int i = 0; i < k_iterations; ++i) {
        const u64 a = random.next_double();
        const u64 b = (i % 3 == 0) ? random.nearby(a) : random.next_double();
        const u64 c = random.next_double();
        const volatile double x = as_double(a), y = as_double(b), z = as_double(c);

        for (const Rounding rounding : k_roundings) {
            const Environment environment{rounding};
            auto truncated = [&](std::function<double()> f) {
                return on_host(Rounding::toward_zero, f);
            };

            if (!(is_infinity(a) && is_infinity(b) && is_negative(a) != is_negative(b))) {
                Flags flags;
                const u64 sum = add(a, b, k_double, environment, flags);
                check("add", a, b, 0, rounding, sum, flags,
                      on_host(rounding, [&] { return x + y; }), truncated([&] { return x + y; }));
            }
            if (!((is_infinity(a) && is_zero(b)) || (is_zero(a) && is_infinity(b)))) {
                Flags flags;
                const u64 product = multiply(a, b, k_double, environment, flags);
                check("multiply", a, b, 0, rounding, product, flags,
                      on_host(rounding, [&] { return x * y; }), truncated([&] { return x * y; }));
            }
            if (!(is_zero(a) && is_zero(b)) && !(is_infinity(a) && is_infinity(b))) {
                Flags flags;
                const u64 quotient = divide(a, b, k_double, environment, flags);
                check("divide", a, b, 0, rounding, quotient, flags,
                      on_host(rounding, [&] { return x / y; }), truncated([&] { return x / y; }));
            }
            {
                const u64 positive = a & ~k_sign_bit;
                const volatile double p = as_double(positive);
                Flags flags;
                const u64 root = square_root(positive, k_double, environment, flags);
                check("square_root", positive, 0, 0, rounding, root, flags,
                      on_host(rounding, [&] { return std::sqrt(p); }),
                      truncated([&] { return std::sqrt(p); }));
            }
            const bool product_infinite = is_infinity(a) || is_infinity(c);
            const bool infinity_times_zero =
                (is_infinity(a) && is_zero(c)) || (is_zero(a) && is_infinity(c));
            const bool opposite_infinities = product_infinite && is_infinity(b) &&
                                             (is_negative(a) != is_negative(c)) != is_negative(b);
            if (!infinity_times_zero && !opposite_infinities) {
                Flags flags;
                const u64 fused = multiply_add(a, c, b, k_double, environment, flags);
                check("multiply_add", a, b, c, rounding, fused, flags,
                      on_host(rounding, [&] { return std::fma(x, z, y); }),
                      truncated([&] { return std::fma(x, z, y); }));
            }
        }
    }
}

// ---- Single precision ----

TEST_CASE("soft_float single arithmetic matches the host") {
    RandomValues random(2);
    for (int i = 0; i < k_iterations; ++i) {
        const u64 a = random.next_single();
        const u64 b = random.next_single();
        const u64 c = random.next_single();
        const volatile float x = as_float(a), y = as_float(b), z = as_float(c);
        auto widen = [](float f) { return static_cast<double>(f); };

        for (const Rounding rounding : k_roundings) {
            const Environment environment{rounding};
            auto truncated = [&](std::function<double()> f) {
                return on_host(Rounding::toward_zero, f);
            };

            if (!(is_infinity(a) && is_infinity(b) && is_negative(a) != is_negative(b))) {
                Flags flags;
                const u64 sum = add(a, b, k_single, environment, flags);
                check("adds", a, b, 0, rounding, sum, flags,
                      on_host(rounding, [&] { return widen(x + y); }),
                      truncated([&] { return widen(x + y); }));
            }
            if (!((is_infinity(a) && is_zero(b)) || (is_zero(a) && is_infinity(b)))) {
                Flags flags;
                const u64 product = multiply(a, b, k_single, environment, flags);
                check("multiplys", a, b, 0, rounding, product, flags,
                      on_host(rounding, [&] { return widen(x * y); }),
                      truncated([&] { return widen(x * y); }));
            }
            if (!(is_zero(a) && is_zero(b)) && !(is_infinity(a) && is_infinity(b))) {
                Flags flags;
                const u64 quotient = divide(a, b, k_single, environment, flags);
                check("divides", a, b, 0, rounding, quotient, flags,
                      on_host(rounding, [&] { return widen(x / y); }),
                      truncated([&] { return widen(x / y); }));
            }
            {
                const u64 positive = a & ~k_sign_bit;
                const volatile float p = as_float(positive);
                Flags flags;
                const u64 root = square_root(positive, k_single, environment, flags);
                check("square_roots", positive, 0, 0, rounding, root, flags,
                      on_host(rounding, [&] { return widen(std::sqrt(p)); }),
                      truncated([&] { return widen(std::sqrt(p)); }));
            }
            const bool infinity_times_zero =
                (is_infinity(a) && is_zero(c)) || (is_zero(a) && is_infinity(c));
            const bool opposite_infinities = (is_infinity(a) || is_infinity(c)) && is_infinity(b) &&
                                             (is_negative(a) != is_negative(c)) != is_negative(b);
            if (!infinity_times_zero && !opposite_infinities) {
                Flags flags;
                const u64 fused = multiply_add(a, c, b, k_single, environment, flags);
                check("multiply_adds", a, b, c, rounding, fused, flags,
                      on_host(rounding, [&] { return widen(std::fma(x, z, y)); }),
                      truncated([&] { return widen(std::fma(x, z, y)); }));
            }
        }
    }
}

// ---- Conversions ----

TEST_CASE("soft_float conversions match the host") {
    RandomValues random(3);
    std::mt19937_64 integers(4);
    for (int i = 0; i < k_iterations; ++i) {
        const u64 a = random.next_double();
        const volatile double x = as_double(a);
        const s64 n = static_cast<s64>(integers()) >> (integers() % 64);
        const volatile s64 volatile_n = n;

        for (const Rounding rounding : k_roundings) {
            {
                Flags flags;
                const u64 single = round_to_format(a, k_single, {rounding}, flags);
                check("round_to_format", a, 0, 0, rounding, single, flags,
                      on_host(rounding, [&] { return static_cast<double>(static_cast<float>(x)); }),
                      on_host(Rounding::toward_zero,
                              [&] { return static_cast<double>(static_cast<float>(x)); }));
            }
            {
                Flags flags;
                const u64 converted = from_integer(n, k_double, {rounding}, flags);
                check("from_integer", static_cast<u64>(n), 0, 0, rounding, converted, flags,
                      on_host(rounding, [&] { return static_cast<double>(volatile_n); }),
                      on_host(Rounding::toward_zero,
                              [&] { return static_cast<double>(volatile_n); }));
            }
            // Rounding to an integer is checked with floor, ceil and trunc, which don't depend on
            // the FPU's rounding mode. (Some compilers ignore the mode for nearbyint and llrint.)
            const double host_integral = integral_on_host(x, rounding);
            {
                INFO(std::hex << a);
                REQUIRE(round_to_integral(a, rounding) == std::bit_cast<u64>(host_integral));
            }
            if (std::isfinite(x) && std::fabs(x) < 9.2e18) {
                Flags flags;
                const IntegerResult ours = to_integer(a, 64, rounding, flags);
                INFO(std::hex << a);
                REQUIRE_FALSE(ours.out_of_range);
                REQUIRE(ours.value == static_cast<s64>(host_integral));
            }
        }
    }
}

TEST_CASE("to_integer saturates out-of-range values") {
    Flags flags;
    const u64 huge = std::bit_cast<u64>(1e300);
    CHECK(to_integer(huge, 64, Rounding::nearest_even, flags).value == INT64_MAX);
    CHECK(to_integer(negate(huge), 32, Rounding::nearest_even, flags).value == INT32_MIN);
    CHECK(to_integer(k_positive_infinity, 32, Rounding::nearest_even, flags).out_of_range);
    // -2^31 fits a 32-bit integer; +2^31 doesn't.
    CHECK_FALSE(to_integer(std::bit_cast<u64>(-2147483648.0), 32, Rounding::toward_zero, flags)
                    .out_of_range);
    CHECK(to_integer(std::bit_cast<u64>(2147483648.0), 32, Rounding::toward_zero, flags)
              .out_of_range);
    // 2147483647.5 rounds up out of range, but truncates into it.
    const u64 almost = std::bit_cast<u64>(2147483647.5);
    CHECK(to_integer(almost, 32, Rounding::nearest_even, flags).out_of_range);
    CHECK(to_integer(almost, 32, Rounding::toward_zero, flags).value == INT32_MAX);
}

// ---- Underflow: PowerPC detects "tiny" before rounding ----
// Expected values worked out by hand from Power ISA v3.1 section 4.4.4.

TEST_CASE("a result that is tiny before rounding underflows, even if it rounds to normal") {
    // (1 - 2^-53) × 2^-1022 = 2^-1022 - 2^-1075: below the smallest normal, exactly halfway
    // between two denormals. Rounding to nearest even carries it up to 2^-1022.
    const u64 almost_one = 0x3fef'ffff'ffff'ffff;
    const u64 smallest_normal = 0x0010'0000'0000'0000;
    Flags flags;
    CHECK(multiply(almost_one, smallest_normal, k_double, {}, flags) == smallest_normal);
    CHECK(flags.underflow);
    CHECK(flags.inexact);
    CHECK(flags.fraction_rounded);
}

TEST_CASE("an exact denormal result does not underflow") {
    Flags flags;
    const u64 half = 0x3fe0'0000'0000'0000;
    CHECK(multiply(0x0010'0000'0000'0000, half, k_double, {}, flags) == 0x0008'0000'0000'0000);
    CHECK_FALSE(flags.underflow);
    CHECK_FALSE(flags.inexact);
}

TEST_CASE("single precision underflows below 2^-126") {
    const u64 almost_one =
        std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(0x3f7fffffu)));
    const u64 smallest_normal = 0x3810'0000'0000'0000; // 2^-126
    Flags flags;
    CHECK(multiply(almost_one, smallest_normal, k_single, {}, flags) == smallest_normal);
    CHECK(flags.underflow);
}

TEST_CASE("enabled underflow wraps the exponent instead of denormalizing") {
    // 2^-1022 - 2^-1075 = (2^53 - 1) × 2^-1075 fits in 53 bits, so normalized it is exact. Its
    // exponent, -1023, plus 1536 is 513: biased 0x600.
    Environment environment;
    environment.underflow_enabled = true;
    Flags flags;
    CHECK(multiply(0x3fef'ffff'ffff'ffff, 0x0010'0000'0000'0000, k_double, environment, flags) ==
          0x600f'ffff'ffff'ffff);
    CHECK(flags.underflow);
    CHECK_FALSE(flags.inexact);
}

TEST_CASE("overflow gives infinity or the largest number, or wraps when enabled") {
    const u64 largest = 0x7fef'ffff'ffff'ffff;
    const u64 two = 0x4000'0000'0000'0000;

    Flags nearest;
    CHECK(multiply(largest, two, k_double, {}, nearest) == k_positive_infinity);
    CHECK(nearest.overflow);
    CHECK(nearest.inexact);

    Flags truncated;
    CHECK(multiply(largest, two, k_double, {Rounding::toward_zero}, truncated) == largest);

    // Enabled: the exact product 2^1024 × (1 - 2^-53) has exponent 1024; minus 1536 is -512.
    Environment environment;
    environment.overflow_enabled = true;
    Flags wrapped;
    CHECK(multiply(largest, two, k_double, environment, wrapped) == 0x1fff'ffff'ffff'ffff);
    CHECK(wrapped.overflow);
    CHECK_FALSE(wrapped.inexact);
}

TEST_CASE("is_denormal depends on the format") {
    CHECK(is_denormal(0x0000'0000'0000'0001, k_double));
    CHECK_FALSE(is_denormal(0x0010'0000'0000'0000, k_double));
    CHECK(is_denormal(0x3800'0000'0000'0000, k_single)); // 2^-127: normal as a double
    CHECK_FALSE(is_denormal(0, k_single));
}
