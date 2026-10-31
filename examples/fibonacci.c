// Prints the first Fibonacci numbers, computed recursively.

#include "lv2.h"

static u64 fibonacci(u64 n) {
    return n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
}

int main(void) {
    for (u64 n = 0; n <= 20; n++) {
        print("fibonacci(");
        print_number(n);
        print(") = ");
        print_number(fibonacci(n));
        print("\n");
    }
    return 0;
}
