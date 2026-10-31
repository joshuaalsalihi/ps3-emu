#pragma once

#include "common/endian.h"
#include "common/types.h"
#include "common/vector.h"
#include "cpu/spu/state.h"

#include <concepts>
#include <span>
#include <vector>

namespace spu {

// An SPU's private 256 KB memory, holding both its code and its data. It isn't part of the
// guest address space (the MFC copies data in and out), so it has its own accessors. Every
// address wraps around, as the hardware masks it with LSLR.
class LocalStore {
public:
    LocalStore() : bytes_(k_local_store_size) {}

    // Scalar accesses are aligned down to the size of T.
    template <std::unsigned_integral T>
    T read(u32 address) const {
        return load_be<T>(&bytes_[address & k_lslr & ~static_cast<u32>(sizeof(T) - 1)]);
    }

    template <std::unsigned_integral T>
    void write(u32 address, T value) {
        store_be<T>(&bytes_[address & k_lslr & ~static_cast<u32>(sizeof(T) - 1)], value);
    }

    // Quadword accesses ignore the low four address bits, as every SPU load and store does.
    Vector read_quad(u32 address) const {
        Vector v;
        const u32 start = address & k_lslr & ~u32{15};
        for (u32 i = 0; i < 16; ++i) {
            v.bytes[i] = bytes_[start + i];
        }
        return v;
    }

    void write_quad(u32 address, const Vector& v) {
        const u32 start = address & k_lslr & ~u32{15};
        for (u32 i = 0; i < 16; ++i) {
            bytes_[start + i] = v.bytes[i];
        }
    }

    std::span<u8> bytes() { return bytes_; }
    std::span<const u8> bytes() const { return bytes_; }

private:
    std::vector<u8> bytes_;
};

} // namespace spu
