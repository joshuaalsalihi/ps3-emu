// A PSL1GHT "hello world": printf goes through newlib, PSL1GHT's runtime and the LV2 kernel.

#include <stdio.h>

int main(void) {
    printf("Hello from PSL1GHT!\n");
    return 0;
}
