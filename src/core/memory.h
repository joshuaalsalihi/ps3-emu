#pragma once

#include "common/endian.h"
#include "common/types.h"
#include "platform/host_memory.h"

#include <concepts>
#include <span>
#include <stdexcept>
#include <vector>

// Thrown when the guest touches memory it may not access.
class MemoryFault : public std::runtime_error {
public:
    MemoryFault(u64 address, bool is_write);

    u64 address;
    bool is_write;
};

// The guest's 32-bit address space. Every guest access goes through this class, which handles
// byte order, page permissions and atomic reservations.
class Memory {
public:
    static constexpr u64 k_size = u64{1} << 32;
    static constexpr u64 k_page_size = 0x1000;
    // Cache line size, which is also the reservation granule for atomics.
    // Cell BE Programming Handbook, "PPE Caches".
    static constexpr u64 k_line_size = 128;

    // Page permissions.
    static constexpr u8 k_read = 1;
    static constexpr u8 k_write = 2;
    static constexpr u8 k_read_write = k_read | k_write;

    Memory();

    // Makes a page-aligned range accessible. Newly mapped memory is zero-filled.
    void map(u64 address, u64 size, u8 permissions);
    // Changes the permissions of an already mapped, page-aligned range.
    void protect(u64 address, u64 size, u8 permissions);
    bool is_accessible(u64 address, u64 size, u8 permissions) const;

    template <std::unsigned_integral T>
    T read(u64 address) const {
        check_access(address, sizeof(T), k_read);
        return load_be<T>(host_.data() + address);
    }

    template <std::unsigned_integral T>
    void write(u64 address, T value) {
        check_access(address, sizeof(T), k_write);
        lose_reservations(address, sizeof(T));
        store_be<T>(host_.data() + address, value);
    }

    void read_bytes(u64 address, std::span<u8> out) const;
    void write_bytes(u64 address, std::span<const u8> bytes);

    // Atomic reservations, used by load-and-reserve / store-conditional pairs such as lwarx and
    // stwcx. A reservation covers one cache line and is lost when anyone writes to that line.
    void reserve(u32 owner, u64 address);
    bool has_reservation(u32 owner, u64 address) const;
    void clear_reservation(u32 owner);

private:
    struct Reservation {
        u32 owner;
        u64 line;
    };

    void check_access(u64 address, u64 size, u8 permissions) const;
    void lose_reservations(u64 address, u64 size);
    void set_permissions(u64 address, u64 size, u8 permissions);

    HostMemory host_;
    std::vector<u8> page_permissions_; // one entry per page
    std::vector<Reservation> reservations_;
};
