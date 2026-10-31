#include "loader/elf.h"

#include "support/elf_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <vector>

using namespace loader;
using Catch::Matchers::ContainsSubstring;
using test::make_elf;

namespace {

const ElfSegment k_code_segment = {
    .type = k_segment_load,
    .flags = k_segment_flag_r | k_segment_flag_x,
    .file_offset = 0x78,
    .vaddr = 0x10000,
    .file_size = 0x80,
    .mem_size = 0x80,
    .align = 0x10000,
};

const ElfSegment k_data_segment = {
    .type = k_segment_load,
    .flags = k_segment_flag_r | k_segment_flag_w,
    .file_offset = 0xf8,
    .vaddr = 0x20000,
    .file_size = 0x10,
    .mem_size = 0x400, // the rest is zero-filled (.bss)
    .align = 0x10000,
};

} // namespace

TEST_CASE("parse_elf reads the header and segments") {
    const ElfFile elf = parse_elf(make_elf({k_code_segment, k_data_segment}));

    CHECK(elf.type == k_elf_type_exec);
    CHECK(elf.machine == k_elf_machine_ppc64);
    CHECK(elf.entry == 0x10200);

    REQUIRE(elf.segments.size() == 2);
    CHECK(elf.segments[0].vaddr == 0x10000);
    CHECK(elf.segments[0].flags == (k_segment_flag_r | k_segment_flag_x));
    CHECK(elf.segments[1].file_offset == 0xf8);
    CHECK(elf.segments[1].mem_size == 0x400);
}

TEST_CASE("parse_elf accepts a file with no segments") {
    CHECK(parse_elf(make_elf({})).segments.empty());
}

TEST_CASE("parse_elf rejects invalid files") {
    std::vector<u8> bytes = make_elf({k_code_segment});

    SECTION("too small") {
        bytes.resize(10);
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("too small"));
    }
    SECTION("wrong magic") {
        bytes[1] = 'X';
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("not an ELF"));
    }
    SECTION("SELF container") {
        bytes[0] = 'S', bytes[1] = 'C', bytes[2] = 'E', bytes[3] = 0;
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("SELF"));
    }
    SECTION("32-bit") {
        bytes[4] = 1;
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("64-bit"));
    }
    SECTION("little-endian") {
        bytes[5] = 1;
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("big-endian"));
    }
    SECTION("wrong machine") {
        bytes[19] = 23; // SPU
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("machine type 23"));
    }
    SECTION("program header table past end of file") {
        bytes.resize(64 + 20);
        CHECK_THROWS_WITH(parse_elf(bytes), ContainsSubstring("program header table"));
    }
}

TEST_CASE("parse_elf rejects inconsistent segments") {
    SECTION("data past end of file") {
        ElfSegment segment = k_code_segment;
        segment.file_size = 0x1000;
        segment.mem_size = 0x1000;
        CHECK_THROWS_WITH(parse_elf(make_elf({segment})), ContainsSubstring("past the end"));
    }
    SECTION("offset overflow") {
        ElfSegment segment = k_code_segment;
        segment.file_offset = ~u64{0};
        CHECK_THROWS_WITH(parse_elf(make_elf({segment})), ContainsSubstring("past the end"));
    }
    SECTION("file size larger than memory size") {
        ElfSegment segment = k_code_segment;
        segment.mem_size = 0x10;
        CHECK_THROWS_WITH(parse_elf(make_elf({segment})), ContainsSubstring("larger in the file"));
    }
}

TEST_CASE("segment names and flags") {
    CHECK(segment_type_name(k_segment_load) == "LOAD");
    CHECK(segment_type_name(k_segment_proc_param) == "PROC_PARAM");
    CHECK(segment_type_name(0x12345678) == "0x12345678");

    CHECK(segment_flags_string(k_segment_flag_r | k_segment_flag_x) == "R-X");
    CHECK(segment_flags_string(0) == "---");
    CHECK(segment_flags_string(0x00400004) == "R--"); // PS3-specific high bits are ignored
}

TEST_CASE("describe_elf prints a segment table") {
    const ElfFile elf = parse_elf(make_elf({k_code_segment, k_data_segment}));

    CHECK(describe_elf(elf) ==
          "Type:    EXEC (executable)\n"
          "Entry:   0x00010200 (function descriptor)\n"
          "\n"
          "Segments (2):\n"
          "  #   Type         Offset       Address      File size    Mem size     Flags\n"
          "  0   LOAD         0x78         0x10000      0x80         0x80         R-X\n"
          "  1   LOAD         0xf8         0x20000      0x10         0x400        RW-\n");
}
