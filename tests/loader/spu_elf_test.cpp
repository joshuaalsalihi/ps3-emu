#include "loader/spu_elf.h"

#include "support/spu_elf_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace loader;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr u64 k_address = 0x20000;

class SpuElfTest {
public:
    SpuElfTest() { memory.map(k_address, 0x1000, Memory::k_read_write); }

    SpuElf read(const std::vector<u8>& file) {
        memory.write_bytes(k_address, file);
        return read_spu_elf(memory, static_cast<u32>(k_address));
    }

    Memory memory;
};

} // namespace

TEST_CASE_METHOD(SpuElfTest, "an SPU ELF's entry point and LOAD segments are read") {
    const SpuElf elf = read(test::make_spu_elf(
        {{0x0, {1, 2, 3, 4, 5, 6, 7, 8}, 8}, {0x800, {9, 9, 9, 9}, 0x40}}, 0xb8));
    CHECK(elf.entry == 0xb8);
    REQUIRE(elf.segments.size() == 2);
    CHECK(elf.segments[0].file_offset == 52 + 2 * 32);
    CHECK(elf.segments[0].file_size == 8);
    CHECK(elf.segments[1].local_address == 0x800);
    CHECK(elf.segments[1].file_offset == 52 + 2 * 32 + 8);
    CHECK(elf.segments[1].memory_size == 0x40);
}

TEST_CASE_METHOD(SpuElfTest, "files that aren't SPU programs are rejected") {
    CHECK_THROWS_WITH(read({'n', 'o', 'p', 'e', 0, 0, 0, 0}), ContainsSubstring("ELF"));
    CHECK_THROWS_WITH(read(test::make_spu_elf({}, 0, k_elf_machine_ppc64)),
                      ContainsSubstring("not an SPU program"));
}

TEST_CASE_METHOD(SpuElfTest, "segments must fit in local store") {
    CHECK_THROWS_WITH(read(test::make_spu_elf({{0x3fff0, {}, 0x20}})),
                      ContainsSubstring("local store"));
    CHECK_THROWS_WITH(read(test::make_spu_elf({{0x0, {1, 2, 3, 4}, 2}})),
                      ContainsSubstring("local store"));
    CHECK_THROWS_WITH(read(test::make_spu_elf({}, 0x40000)), ContainsSubstring("entry point"));
}
