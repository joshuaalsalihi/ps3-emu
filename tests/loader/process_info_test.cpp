#include "loader/process_info.h"

#include "support/import_builder.h"

#include <catch2/catch_test_macros.hpp>

using namespace loader;

namespace {

constexpr u64 k_data = 0x20000;

struct ProcessInfoTest {
    ProcessInfoTest() { memory.map(k_data, Memory::k_page_size, Memory::k_read_write); }

    ElfFile elf_with_segment(u32 type, u64 vaddr, u64 size) {
        ElfFile elf;
        elf.segments.push_back({.type = type, .vaddr = vaddr, .file_size = size});
        return elf;
    }

    Memory memory;
};

} // namespace

TEST_CASE_METHOD(ProcessInfoTest, "read_imports lists each imported function and its stub") {
    const auto tables =
        test::write_import_tables(memory, k_data, "sysPrxForUser", {0xe6f2c1e7, 0x1573dc3f});
    const auto imports = read_imports(elf_with_segment(k_segment_prx_param, k_data, 0x28), memory);

    REQUIRE(imports.size() == 2);
    CHECK(imports[0].module == "sysPrxForUser");
    CHECK(imports[0].nid == 0xe6f2c1e7);
    CHECK(imports[0].stub_address == tables.stub_slots[0]);
    CHECK(imports[1].nid == 0x1573dc3f);
    CHECK(imports[1].stub_address == tables.stub_slots[1]);
}

TEST_CASE_METHOD(ProcessInfoTest, "programs without a PRX_PARAM segment import nothing") {
    CHECK(read_imports(ElfFile{}, memory).empty());
}

TEST_CASE_METHOD(ProcessInfoTest, "a PRX_PARAM block with the wrong magic is rejected") {
    test::write_import_tables(memory, k_data, "sysPrxForUser", {1});
    memory.write<u32>(k_data + 4, 0x12345678);
    CHECK_THROWS_AS(read_imports(elf_with_segment(k_segment_prx_param, k_data, 0x28), memory),
                    LoadError);
}

TEST_CASE_METHOD(ProcessInfoTest, "read_process_param reads the main thread's settings") {
    memory.write<u32>(k_data + 4, 0x13bcc5f6); // magic
    memory.write<u32>(k_data + 16, 1001);      // priority
    memory.write<u32>(k_data + 20, 0x10'0000); // stack size
    const auto param =
        read_process_param(elf_with_segment(k_segment_proc_param, k_data, 32), memory);
    REQUIRE(param);
    CHECK(param->primary_priority == 1001);
    CHECK(param->primary_stack_size == 0x10'0000);

    memory.write<u32>(k_data + 4, 0);
    CHECK_FALSE(read_process_param(elf_with_segment(k_segment_proc_param, k_data, 32), memory));
}
