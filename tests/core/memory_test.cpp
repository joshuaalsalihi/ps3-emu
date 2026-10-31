#include "core/memory.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("unmapped memory faults") {
    Memory memory;
    CHECK_THROWS_AS(memory.read<u32>(0x10000), MemoryFault);
    CHECK_THROWS_AS(memory.write<u32>(0x10000, 1), MemoryFault);
    CHECK_FALSE(memory.is_accessible(0x10000, 4, Memory::k_read));
}

TEST_CASE("mapped memory starts zeroed and stores big-endian") {
    Memory memory;
    memory.map(0x10000, Memory::k_page_size, Memory::k_read_write);
    CHECK(memory.read<u64>(0x10000) == 0);

    memory.write<u32>(0x10000, 0x11223344);
    CHECK(memory.read<u8>(0x10000) == 0x11);
    CHECK(memory.read<u16>(0x10002) == 0x3344);
}

TEST_CASE("accesses may cross into the next page only if it is mapped") {
    Memory memory;
    memory.map(0x10000, Memory::k_page_size, Memory::k_read_write);
    CHECK_THROWS_AS(memory.read<u32>(0x10ffe), MemoryFault);

    memory.map(0x11000, Memory::k_page_size, Memory::k_read_write);
    memory.write<u32>(0x10ffe, 0xaabbccdd);
    CHECK(memory.read<u16>(0x11000) == 0xccdd);
}

TEST_CASE("permissions are enforced") {
    Memory memory;
    memory.map(0x10000, Memory::k_page_size, Memory::k_read_write);
    memory.protect(0x10000, Memory::k_page_size, Memory::k_read);

    CHECK_NOTHROW(memory.read<u32>(0x10000));
    CHECK_THROWS_AS(memory.write<u32>(0x10000, 1), MemoryFault);

    const std::array<u8, 4> bytes{1, 2, 3, 4};
    CHECK_THROWS_AS(memory.write_bytes(0x10000, bytes), MemoryFault);
}

TEST_CASE("faults report the address and direction") {
    Memory memory;
    try {
        memory.write<u8>(0x1234, 0);
        FAIL("expected a fault");
    } catch (const MemoryFault& fault) {
        CHECK(fault.address == 0x1234);
        CHECK(fault.is_write);
    }
}

TEST_CASE("map and protect require page-aligned ranges below 4 GB") {
    Memory memory;
    CHECK_THROWS_AS(memory.map(0x10001, Memory::k_page_size, Memory::k_read),
                    std::invalid_argument);
    CHECK_THROWS_AS(memory.map(0x10000, 100, Memory::k_read), std::invalid_argument);
    CHECK_THROWS_AS(memory.map(Memory::k_size, Memory::k_page_size, Memory::k_read),
                    std::invalid_argument);
}

TEST_CASE("reservations are lost when anyone writes to the line") {
    Memory memory;
    memory.map(0x10000, Memory::k_page_size, Memory::k_read_write);
    memory.reserve(1, 0x10010);
    memory.reserve(2, 0x10010);

    CHECK(memory.has_reservation(1, 0x10070)); // same 128-byte line
    CHECK_FALSE(memory.has_reservation(1, 0x10080));

    memory.write<u8>(0x10080, 1); // next line: no effect
    CHECK(memory.has_reservation(1, 0x10010));

    memory.write<u8>(0x1007f, 1); // last byte of the reserved line
    CHECK_FALSE(memory.has_reservation(1, 0x10010));
    CHECK_FALSE(memory.has_reservation(2, 0x10010));
}

TEST_CASE("a new reservation replaces the old one") {
    Memory memory;
    memory.reserve(1, 0x10000);
    memory.reserve(1, 0x20000);
    CHECK_FALSE(memory.has_reservation(1, 0x10000));
    CHECK(memory.has_reservation(1, 0x20000));

    memory.clear_reservation(1);
    CHECK_FALSE(memory.has_reservation(1, 0x20000));
}
