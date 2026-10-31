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

// Reverses the byte order of `value`.
template <std::unsigned_integral T>
T byte_swap(T value) {
    T result = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        result = static_cast<T>((result << 8) | ((value >> (8 * i)) & 0xff));
    }
    return result;
}

// Writes `value` to `bytes` in big-endian order. The caller must ensure sizeof(T) bytes are
// writable.
template <std::unsigned_integral T>
void store_be(u8* bytes, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[i] = static_cast<u8>(value >> (8 * (sizeof(T) - 1 - i)));
    }
}
