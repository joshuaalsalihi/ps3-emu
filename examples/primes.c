// Finds the primes below 1000 with the sieve of Eratosthenes.

#include "lv2.h"

enum { k_limit = 1000 };

// Zero-initialized (.bss), so this also checks that the loader clears it.
static unsigned char is_composite[k_limit];

int main(void) {
    for (u64 n = 2; n * n < k_limit; n++) {
        if (is_composite[n]) continue;
        for (u64 multiple = n * n; multiple < k_limit; multiple += n) {
            is_composite[multiple] = 1;
        }
    }

    u64 count = 0;
    for (u64 n = 2; n < k_limit; n++) {
        if (is_composite[n]) continue;
        print_number(n);
        print(++count % 10 == 0 ? "\n" : " ");
    }
    print("\n");
    print_number(count);
    print(" primes below 1000\n");
    return 0;
}
