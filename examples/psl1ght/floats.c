// printf of floating-point values. newlib's number formatting exercises much of the FPU:
// multiplies, divides, conversions to and from integers, and exact rounding.

#include <math.h>
#include <stdio.h>

static volatile double values[] = {3.141592653589793, 1.0 / 3.0, 1e-10, 6.02214076e23, -2.5,
                                   0.1 + 0.2,         2.0,       1e300, 4.9e-324};

int main(void) {
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
        const double value = values[i];
        printf("%.17g  %.3f  %e  sqrt=%.17g\n", value, value, value, sqrt(fabs(value)));
    }
    volatile float f = 1.1f;
    printf("single: %.9g\n", (double)(f * f));
    return 0;
}
