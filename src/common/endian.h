#pragma once

#include "common/types.h"

#include <concepts>
#include <cstddef>

// Reads a big-endian integer from `bytes`. The caller must ensure sizeof(T) bytes are readable.
template <std::unsigned_integral T>
T load_be(const u8* bytes) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>((value << 8) | bytes[i]);
    }
    return value;
}
