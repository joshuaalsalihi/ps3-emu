#include "loader/program.h"

#include "support/elf_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace loader;
using Catch::Matchers::ContainsSubstring;
using test::make_program;
using test::words_to_bytes;

namespace {

constexpr u32 k_read_execute = k_segment_flag_r | k_segment_flag_x;
constexpr u32 k_read_write = k_segment_flag_r | k_segment_flag_w;

ElfFile parse_and_load(const std::vector<u8>& file, Memory& memory, LoadedProgram& program) {
    ElfFile elf = parse_elf(file);
    program = load_program(elf, file, memory);
    return elf;
}

} // namespace

TEST_CASE("load_program copies segments and reads the entry descriptor") {
    // Code at 0x10000; a descriptor (code address, TOC) plus data at 0x20000.
    const auto file = make_program(
        {
            {0x10000, k_read_execute, words_to_bytes({0x60000000})},
            {0x20000, k_read_write, words_to_bytes({0x10000, 0x28000, 0xcafef00d}), 0x2000},
        },
        0x20000);

    Memory memory;
    LoadedProgram program{};
    parse_and_load(file, memory, program);

    CHECK(program.entry == 0x10000);
    CHECK(program.toc == 0x28000);
    CHECK(memory.read<u32>(0x10000) == 0x60000000);
    CHECK(memory.read<u32>(0x20008) == 0xcafef00d);
    CHECK(memory.read<u32>(0x21ffc) == 0); // beyond the file contents: zero-filled
}

TEST_CASE("segment permissions are applied") {
    const auto file = make_program(
        {
            {0x10000, k_read_execute, words_to_bytes({0x60000000})},
            {0x20000, k_read_write, words_to_bytes({0x10000, 0})},
        },
        0x20000);

    Memory memory;
    LoadedProgram program{};
    parse_and_load(file, memory, program);

    CHECK_THROWS_AS(memory.write<u32>(0x10000, 0), MemoryFault);
    CHECK_NOTHROW(memory.write<u32>(0x20000, 0));
}

TEST_CASE("segments sharing a page combine their permissions") {
    const auto file = make_program(
        {
            {0x10000, k_read_execute, words_to_bytes({0x60000000, 0x10000, 0})},
            {0x10800, k_read_write, words_to_bytes({0x11111111})},
        },
        0x10004);

    Memory memory;
    LoadedProgram program{};
    parse_and_load(file, memory, program);

    CHECK(memory.read<u32>(0x10000) == 0x60000000); // not wiped by mapping the second segment
    CHECK(memory.read<u32>(0x10800) == 0x11111111);
    CHECK(memory.is_accessible(0x10000, 4, Memory::k_read_write));
}

TEST_CASE("load_program rejects bad programs") {
    Memory memory;
    LoadedProgram program{};

    SECTION("entry descriptor outside the loaded segments") {
        const auto file = make_program({{0x10000, k_read_execute, {0, 0, 0, 0}}}, 0x50000);
        CHECK_THROWS_WITH(parse_and_load(file, memory, program), ContainsSubstring("entry"));
    }
    SECTION("segment above 4 GB") {
        const auto file = make_program({{0x1'0000'0000, k_read_write, {0, 0, 0, 0}}}, 0);
        CHECK_THROWS_WITH(parse_and_load(file, memory, program), ContainsSubstring("4 GB"));
    }
}
