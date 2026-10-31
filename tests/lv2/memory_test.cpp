// Kernel memory: sys_memory and the sys_mmapper flow PSL1GHT's malloc uses.

#include "support/kernel_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace lv2;
using test::KernelTest;

namespace {

constexpr u64 k_1m = 0x10'0000;
constexpr u64 k_page_size_1m_flag = 0x400;
constexpr u64 k_page_size_64k_flag = 0x200;

} // namespace

TEST_CASE_METHOD(KernelTest, "sys_memory_allocate maps page-aligned memory") {
    u32 address = 0;
    REQUIRE(kernel.allocate_memory(2 * k_1m, k_page_size_1m_flag, address) == k_ok);
    CHECK(address % k_1m == 0);
    CHECK(address >= Kernel::k_user_memory_base);
    CHECK(memory.is_accessible(address, 2 * k_1m, Memory::k_read_write));
    CHECK(kernel.user_memory_available() == Kernel::k_user_memory_size - 2 * k_1m);

    REQUIRE(kernel.free_memory(address) == k_ok);
    CHECK_FALSE(memory.is_accessible(address, 1, Memory::k_read));
    CHECK(kernel.free_memory(address) == k_error_invalid);
}

TEST_CASE_METHOD(KernelTest, "sys_memory_allocate checks the size against the page size") {
    u32 address = 0;
    CHECK(kernel.allocate_memory(0x1'0000, k_page_size_1m_flag, address) == k_error_alignment);
    CHECK(kernel.allocate_memory(0x1'0000, k_page_size_64k_flag, address) == k_ok);
    CHECK(kernel.allocate_memory(0, k_page_size_64k_flag, address) == k_error_alignment);
}

TEST_CASE_METHOD(KernelTest, "the memory mapper: reserve, create, map, unmap, release") {
    u32 range = 0;
    REQUIRE(kernel.reserve_address_range(0x1000'0000, 0, range) == k_ok);

    u32 block = 0;
    REQUIRE(kernel.create_memory_block(k_1m, k_page_size_1m_flag, block) == k_ok);

    u32 mapped = 0;
    REQUIRE(kernel.search_and_map_memory_block(range, block, mapped) == k_ok);
    CHECK(mapped == range);
    CHECK(memory.is_accessible(mapped, k_1m, Memory::k_read_write));

    // While mapped, neither the block nor the range can go.
    CHECK(kernel.destroy_memory_block(block) == k_error_busy);
    CHECK(kernel.release_address_range(range) == k_error_busy);

    u32 unmapped_block = 0;
    REQUIRE(kernel.unmap_memory_block(mapped, unmapped_block) == k_ok);
    CHECK(unmapped_block == block);
    CHECK_FALSE(memory.is_accessible(mapped, 1, Memory::k_read));
    CHECK(kernel.destroy_memory_block(block) == k_ok);
    CHECK(kernel.release_address_range(range) == k_ok);
}

TEST_CASE_METHOD(KernelTest, "blocks can be mapped at a chosen address inside a range") {
    u32 range = 0;
    REQUIRE(kernel.reserve_address_range(0x1000'0000, 0, range) == k_ok);
    u32 first = 0;
    u32 second = 0;
    REQUIRE(kernel.create_memory_block(k_1m, k_page_size_1m_flag, first) == k_ok);
    REQUIRE(kernel.create_memory_block(k_1m, k_page_size_1m_flag, second) == k_ok);

    CHECK(kernel.map_memory_block(range + k_1m, first) == k_ok);
    CHECK(kernel.map_memory_block(range + k_1m, second) == k_error_busy);        // taken
    CHECK(kernel.map_memory_block(range + 0x1000, second) == k_error_alignment); // not 1 MB
    CHECK(kernel.map_memory_block(0x1000'0000, second) == k_error_invalid);      // not reserved
}

TEST_CASE_METHOD(KernelTest, "address ranges come in 256 MB units") {
    u32 range = 0;
    CHECK(kernel.reserve_address_range(k_1m, 0, range) == k_error_alignment);
}
