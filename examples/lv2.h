// Tiny runtime for the C examples: LV2 syscalls and console printing. There is no C library.
#pragma once

typedef unsigned long u64;
typedef long s64;

// Calls an LV2 syscall: the number goes in r11, arguments in r3-r6, the result comes back in r3.
static inline s64 lv2_syscall(s64 number, s64 a, s64 b, s64 c, s64 d) {
    register s64 r3 __asm__("r3") = a;
    register s64 r4 __asm__("r4") = b;
    register s64 r5 __asm__("r5") = c;
    register s64 r6 __asm__("r6") = d;
    register s64 r11 __asm__("r11") = number;
    __asm__ volatile("sc"
                     : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r11)
                     :
                     : "r0", "r7", "r8", "r9", "r10", "r12", "cr0", "ctr", "xer", "memory");
    return r3;
}

static inline u64 string_length(const char* text) {
    u64 length = 0;
    while (text[length] != 0) {
        length++;
    }
    return length;
}

static inline void print(const char* text) {
    lv2_syscall(403 /* sys_tty_write */, 0, (s64)text, (s64)string_length(text), 0);
}

static inline void print_number(s64 value) {
    char buffer[24];
    int i = sizeof buffer;
    buffer[--i] = 0;

    u64 magnitude = value < 0 ? -(u64)value : (u64)value;
    do {
        buffer[--i] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) buffer[--i] = '-';

    print(&buffer[i]);
}

// Prints all 16 hex digits of a 64-bit value.
static inline void print_hex(u64 value) {
    char buffer[17];
    for (int i = 15; i >= 0; i--) {
        buffer[i] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    buffer[16] = 0;
    print(buffer);
}
