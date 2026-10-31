// Checks that compiler-generated code computes what C says it should. Each check targets
// instructions the compiler picks for that operation (noted in comments). Inputs are read
// through `volatile` so the compiler can't precompute the answers.
//
// Exits with the number of failed checks.

#include "lv2.h"

typedef unsigned int u32;
typedef int s32;
typedef unsigned __int128 u128;

static int checks;
static int failures;

static void check(const char* name, u64 actual, u64 expected) {
    checks++;
    if (actual == expected) return;
    failures++;
    print("FAIL ");
    print(name);
    print(": got ");
    print_number((s64)actual);
    print(", expected ");
    print_number((s64)expected);
    print("\n");
}

// Reads a value the compiler can't see through.
static u64 opaque(u64 value) {
    volatile u64 copy = value;
    return copy;
}

static void check_multiply_divide(void) {
    const u64 a = opaque(0x0123456789abcdef);
    const u64 b = opaque(0xfedcba9876543210);
    check("mulld", a * b, 0x2236d88fe5618cf0);
    check("mulhdu", (u64)(((u128)a * b) >> 64), 0x0121fa00ad77d742);
    check("divdu", b / opaque(7), 0x2468acf13579be02);
    check("divd", (u64)((s64)b / (s64)opaque(-7)), 0x00299c335ccf6690);
    check("modulo", b % opaque(1000), 0x2d0);
    check("divw", (u64)(s64)((s32)opaque(-100) / (s32)opaque(7)), (u64)-14);
    check("divwu", (u32)opaque(0xfffffff0) / (u32)opaque(3), 0x55555550);
    check("mullw", (u64)(s64)((s32)opaque(-30000) * (s32)opaque(70000)), (u64)-2100000000);
}

static void check_shifts_rotates(void) {
    const u64 x = opaque(0x8000000000000081);
    const u64 n = opaque(4);
    check("sld", x << n, 0x0000000000000810);
    check("srd", x >> n, 0x0800000000000008);
    check("srad", (u64)((s64)x >> n), 0xf800000000000008);
    check("slw", (u32)x << n, 0x810);
    check("sraw", (u64)(s64)((s32)opaque(0x80000081) >> n), 0xfffffffff8000008);
    check("rotate left", (x << n) | (x >> (64 - n)), 0x0000000000000818);
    check("shift by 63", opaque(1) << opaque(63), 0x8000000000000000);
}

static void check_bits(void) {
    check("cntlzd", (u64)__builtin_clzll(opaque(0x00f0000000000000)), 8);
    check("cntlzw", (u64)__builtin_clz((u32)opaque(0x00008000)), 16);
    check("bswap32", __builtin_bswap32((u32)opaque(0x11223344)), 0x44332211);
    check("bswap64", __builtin_bswap64(opaque(0x1122334455667788)), 0x8877665544332211);
    check("sign extend byte", (u64)(s64)(signed char)opaque(0x80), 0xffffffffffffff80);
    check("sign extend short", (u64)(s64)(short)opaque(0x8001), 0xffffffffffff8001);
    check("sign extend word", (u64)(s64)(s32)opaque(0x80000000), 0xffffffff80000000);
}

static void check_carry_chain(void) {
    // A 128-bit add uses addc/adde to carry from the low doubleword into the high one.
    const u128 a = ((u128)opaque(1) << 64) | opaque(0xffffffffffffffff);
    const u128 sum = a + opaque(1);
    check("128-bit add (high)", (u64)(sum >> 64), 2);
    check("128-bit add (low)", (u64)sum, 0);

    const u128 difference = ((u128)opaque(2) << 64) - opaque(1);
    check("128-bit subtract (high)", (u64)(difference >> 64), 1);
    check("128-bit subtract (low)", (u64)difference, 0xffffffffffffffff);
}

static void check_atomics(void) {
    // lwarx/stwcx. and ldarx/stdcx. loops.
    static u32 counter32 = 40;
    static u64 counter64 = 100;
    check("atomic add (old value)", __atomic_fetch_add(&counter32, 2, __ATOMIC_SEQ_CST), 40);
    check("atomic add (new value)", counter32, 42);
    check("atomic exchange", __atomic_exchange_n(&counter64, 7, __ATOMIC_SEQ_CST), 100);

    u64 expected = 7;
    const int swapped = __atomic_compare_exchange_n(&counter64, &expected, 9, 0, __ATOMIC_SEQ_CST,
                                                    __ATOMIC_SEQ_CST);
    check("compare-exchange succeeds", (u64)swapped, 1);
    check("compare-exchange stores", counter64, 9);
}

static u64 add_one(u64 x) {
    return x + 1;
}
static u64 double_it(u64 x) {
    return x * 2;
}

static u64 pick(u64 selector) {
    // Dense switches become jump tables: mtctr + bctr.
    switch (selector) {
    case 0: return 11;
    case 1: return 22;
    case 2: return 33;
    case 3: return 44;
    case 4: return 55;
    case 5: return 66;
    default: return 0;
    }
}

static void check_control_flow(void) {
    u64 (*volatile function)(u64) = add_one; // indirect call: mtctr + bctrl
    check("indirect call", function(41), 42);
    function = double_it;
    check("indirect call again", function(21), 42);

    u64 total = 0;
    for (u64 i = 0; i < 7; i++)
        total += pick(opaque(i));
    check("jump table", total, 231);
}

int main(void) {
    check_multiply_divide();
    check_shifts_rotates();
    check_bits();
    check_carry_chain();
    check_atomics();
    check_control_flow();

    print_number(checks - failures);
    print(" of ");
    print_number(checks);
    print(" checks passed\n");
    return failures;
}
