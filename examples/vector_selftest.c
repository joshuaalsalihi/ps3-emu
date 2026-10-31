// Checks vector (VMX) instructions against plain scalar code computing the same results. The
// float checks use values that are exact in every rounding mode, so they hold in both the
// Cell's IEEE and graphics rounding modes.
//
// Exits with the number of failed checks.

#include "lv2.h"

#include <altivec.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;

static int checks;
static int failures;

static void check(const char* name, int ok) {
    checks++;
    if (ok) return;
    failures++;
    print("FAIL ");
    print(name);
    print("\n");
}

// Inputs the compiler can't see through.
static volatile u8 bytes_a[16] __attribute__((aligned(16))) = {0,  1,  2,  127, 128, 200, 255, 9,
                                                               10, 11, 12, 13,  14,  15,  250, 251};
static volatile u8 bytes_b[16]
    __attribute__((aligned(16))) = {255, 254, 100, 1, 128, 100, 1, 7, 3, 4, 5, 6, 7, 8, 9, 10};
static volatile u8 unaligned[48] __attribute__((aligned(16)));

static vector unsigned char load_bytes(volatile u8* source) {
    return vec_ld(0, (u8*)source);
}

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static void check_bytes(void) {
    const vector unsigned char a = load_bytes(bytes_a), b = load_bytes(bytes_b);
    union {
        vector unsigned char v;
        u8 e[16];
    } sum = {vec_add(a, b)}, saturated = {vec_adds(a, b)}, average = {vec_avg(a, b)},
      maximum = {vec_max(a, b)}, difference = {vec_subs(a, b)};
    union {
        vector signed char v;
        s8 e[16];
    } signed_sum = {vec_adds((vector signed char)a, (vector signed char)b)};

    int ok_sum = 1, ok_saturated = 1, ok_average = 1, ok_max = 1, ok_difference = 1, ok_signed = 1;
    for (int i = 0; i < 16; i++) {
        const int x = bytes_a[i], y = bytes_b[i];
        ok_sum &= sum.e[i] == (u8)(x + y);
        ok_saturated &= saturated.e[i] == clamp(x + y, 0, 255);
        ok_average &= average.e[i] == (x + y + 1) / 2;
        ok_max &= maximum.e[i] == (x > y ? x : y);
        ok_difference &= difference.e[i] == clamp(x - y, 0, 255);
        ok_signed &= signed_sum.e[i] == clamp((s8)x + (s8)y, -128, 127);
    }
    check("vec_add bytes", ok_sum);
    check("vec_adds bytes", ok_saturated);
    check("vec_avg bytes", ok_average);
    check("vec_max bytes", ok_max);
    check("vec_subs bytes", ok_difference);
    check("vec_adds signed bytes", ok_signed);
}

static void check_words(void) {
    const vector signed int a = {1, -2, 0x7fffffff, -100};
    const vector signed int b = {10, 20, 1, 3};
    volatile int shift = 3;
    union {
        vector signed int v;
        s32 e[4];
    } product_sum = {vec_msum((vector signed short)a, (vector signed short)b, a)},
      shifted = {vec_sra(a, vec_splats((unsigned int)shift))}, saturated = {vec_adds(a, b)},
      rotated = {vec_rl(a, (vector unsigned int)b)};

    const s32 x[4] = {1, -2, 0x7fffffff, -100}, y[4] = {10, 20, 1, 3};
    int ok_msum = 1, ok_sra = 1, ok_adds = 1, ok_rl = 1;
    for (int i = 0; i < 4; i++) {
        const s16 xh = (s16)((u32)x[i] >> 16), xl = (s16)x[i];
        const s16 yh = (s16)((u32)y[i] >> 16), yl = (s16)y[i];
        ok_msum &= product_sum.e[i] == (s32)((u32)(xh * yh + xl * yl) + (u32)x[i]);
        ok_sra &= shifted.e[i] == x[i] >> 3;
        const long long wide = (long long)x[i] + y[i];
        ok_adds &= saturated.e[i] == (wide > 0x7fffffff ? 0x7fffffff : (s32)wide);
        const u32 n = (u32)y[i] & 31;
        ok_rl &= (u32)rotated.e[i] == (n ? ((u32)x[i] << n) | ((u32)x[i] >> (32 - n)) : (u32)x[i]);
    }
    check("vec_msum halves", ok_msum);
    check("vec_sra words", ok_sra);
    check("vec_adds words", ok_adds);
    check("vec_rl words", ok_rl);
}

static void check_permutes(void) {
    for (int i = 0; i < 48; i++) {
        unaligned[i] = (u8)(i * 3);
    }

    // The classic unaligned load: two aligned loads, then vperm with lvsl's control.
    const u8* p = (const u8*)unaligned + 5;
    const vector unsigned char low = vec_ld(0, p), high = vec_ld(16, p);
    union {
        vector unsigned char v;
        u8 e[16];
    } loaded = {vec_perm(low, high, vec_lvsl(0, p))};
    int ok_unaligned = 1;
    for (int i = 0; i < 16; i++) {
        ok_unaligned &= loaded.e[i] == (u8)((5 + i) * 3);
    }
    check("unaligned load with lvsl and vperm", ok_unaligned);

    const vector unsigned int a = {1, 2, 3, 4}, b = {5, 6, 7, 8};
    union {
        vector unsigned int v;
        u32 e[4];
    } merged = {vec_mergeh(a, b)}, splatted = {vec_splat(b, 2)}, shifted = {vec_sld(a, b, 4)},
      selected = {vec_sel(a, b, (vector unsigned int){0, ~0u, 0, ~0u})};
    check("vec_mergeh",
          merged.e[0] == 1 && merged.e[1] == 5 && merged.e[2] == 2 && merged.e[3] == 6);
    check("vec_splat", splatted.e[0] == 7 && splatted.e[3] == 7);
    check("vec_sld", shifted.e[0] == 2 && shifted.e[3] == 5);
    check("vec_sel", selected.e[0] == 1 && selected.e[1] == 6 && selected.e[3] == 8);

    const vector signed short wide = {-1, 300, -300, 5, 0, 32767, -32768, 7};
    union {
        vector unsigned char v;
        u8 e[16];
    } packed = {vec_packsu(wide, wide)};
    check("vec_packsu", packed.e[0] == 0 && packed.e[1] == 255 && packed.e[2] == 0 &&
                            packed.e[3] == 5 && packed.e[5] == 255);
    union {
        vector signed int v;
        s32 e[4];
    } unpacked = {vec_unpackh(wide)};
    check("vec_unpackh", unpacked.e[0] == -1 && unpacked.e[1] == 300 && unpacked.e[2] == -300);
}

static void check_compares(void) {
    const vector signed int a = {1, 2, 3, 4}, b = {1, 5, 3, 0};
    // The predicates use the record forms, which summarize the comparison in CR6.
    check("vec_all_eq (false)", !vec_all_eq(a, b));
    check("vec_any_eq (true)", vec_any_eq(a, b));
    check("vec_all_eq (true)", vec_all_eq(a, a));
    check("vec_any_gt", vec_any_gt(a, b));
    check("vec_all_ge (false)", !vec_all_ge(a, b));
}

static void check_floats(void) {
    const vector float a = {1.5f, -2.0f, 8.0f, 0.25f}, b = {2.0f, 0.5f, -1.0f, 4.0f};
    const vector float c = {0.5f, 1.0f, 3.0f, -0.25f};
    const vector signed int whole = {3, -4, 100, 0};
    union {
        vector float v;
        float e[4];
    } sum = {vec_add(a, b)}, product = {vec_madd(a, b, c)}, maximum = {vec_max(a, b)},
      floors = {vec_floor((vector float){1.5f, -1.5f, 2.0f, -0.5f})},
      converted = {vec_ctf(whole, 1)};
    union {
        vector signed int v;
        s32 e[4];
    } integers = {vec_cts(a, 2)};

    const float x[4] = {1.5f, -2.0f, 8.0f, 0.25f}, y[4] = {2.0f, 0.5f, -1.0f, 4.0f};
    const float z[4] = {0.5f, 1.0f, 3.0f, -0.25f};
    int ok_add = 1, ok_madd = 1, ok_max = 1;
    for (int i = 0; i < 4; i++) {
        ok_add &= sum.e[i] == x[i] + y[i];
        ok_madd &= product.e[i] == x[i] * y[i] + z[i];
        ok_max &= maximum.e[i] == (x[i] > y[i] ? x[i] : y[i]);
    }
    check("vec_add floats", ok_add);
    check("vec_madd floats", ok_madd);
    check("vec_max floats", ok_max);
    check("vec_floor", floors.e[0] == 1.0f && floors.e[1] == -2.0f && floors.e[3] == -1.0f);
    check("vec_ctf", converted.e[0] == 1.5f && converted.e[1] == -2.0f && converted.e[2] == 50.0f);
    check("vec_cts", integers.e[0] == 6 && integers.e[1] == -8 && integers.e[3] == 1);
}

int main(void) {
    check_bytes();
    check_words();
    check_permutes();
    check_compares();
    check_floats();

    print_number(checks - failures);
    print(" of ");
    print_number(checks);
    print(" vector checks passed\n");
    return failures;
}
