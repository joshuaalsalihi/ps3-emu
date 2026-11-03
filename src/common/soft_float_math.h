#pragma once

// Functions IEEE 754 doesn't round exactly, computed from soft_float's exactly rounded operations
// so every host gets the same bits. They are as accurate as the estimates and approximations
// that call them need, not correctly rounded.

#include "common/types.h"

namespace soft_float {

// log2 of a positive, finite, normal double, to 24 bits after the binary point (truncated).
u64 log2(u64 x);

// 2^f for a double f in [0, 1), from f's top 24 fraction bits.
u64 exp2_fraction(u64 f);

} // namespace soft_float
