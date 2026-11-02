// The import linker, and the sysPrxForUser functions it dispatches to.

#include "hle/linker.h"
#include "hle/nid.h"

#include "support/import_builder.h"
#include "support/kernel_test.h"
#include "support/spu_elf_builder.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace lv2;
using test::KernelTest;

namespace {

constexpr u64 k_tables = 0x30'000; // import tables live on their own page
constexpr u32 k_unknown_nid = 0x12345678;

// Links imports of the named sysPrxForUser functions, then calls them from a thread.
struct LinkerTest : KernelTest {
    LinkerTest() {
        memory.map(k_tables, Memory::k_page_size, Memory::k_read_write);
        std::vector<u32> nids;
        for (const std::string_view name : k_names) {
            nids.push_back(hle::nid(name));
        }
        nids.push_back(k_unknown_nid);
        tables = test::write_import_tables(memory, k_tables, "sysPrxForUser", nids);

        std::vector<loader::ImportedFunction> imports;
        for (std::size_t i = 0; i < nids.size(); ++i) {
            imports.push_back({"sysPrxForUser", nids[i], static_cast<u32>(tables.stub_slots[i])});
        }
        linker.link(imports, memory);
    }

    // Calls an import (by its index in k_names) with `args` in r3, r4, ...
    void call(PpuThread& thread, std::size_t index, std::initializer_list<u64> args) {
        u32 reg = 3;
        for (const u64 arg : args) {
            thread.cpu.state.gpr[reg++] = arg;
        }
        linker.call(static_cast<u32>(index), kernel, thread);
    }

    static constexpr std::string_view k_names[] = {
        "sys_lwmutex_create",   "sys_lwmutex_lock",      "sys_lwmutex_unlock",
        "sys_lwcond_create",    "sys_lwcond_wait",       "sys_lwcond_signal",
        "sys_spinlock_lock",    "sys_ppu_thread_get_id", "sys_process_exit",
        "sys_spu_image_import", "sys_spu_image_close",
    };
    enum Index {
        lwmutex_create,
        lwmutex_lock,
        lwmutex_unlock,
        lwcond_create,
        lwcond_wait,
        lwcond_signal,
        spinlock_lock,
        thread_get_id,
        process_exit,
        spu_image_import,
        spu_image_close,
        unknown
    };

    test::ImportTables tables;
    hle::Linker linker;
};

constexpr u64 k_lwmutex = KernelTest::k_data;
constexpr u64 k_lwmutex_attr = KernelTest::k_data + 0x40;
constexpr u64 k_lwcond = KernelTest::k_data + 0x80;

} // namespace

TEST_CASE_METHOD(LinkerTest, "each stub points at a descriptor for an `hle n; blr` thunk") {
    for (std::size_t i = 0; i < tables.stub_slots.size(); ++i) {
        const u32 descriptor = memory.read<u32>(tables.stub_slots[i]);
        const u32 thunk = memory.read<u32>(descriptor);
        CHECK(memory.read<u32>(thunk) == (0x04000000 | i)); // hle i
        CHECK(memory.read<u32>(thunk + 4) == 0x4e800020);   // blr
    }
}

TEST_CASE_METHOD(LinkerTest, "firmware-internal functions get thunks at fixed addresses") {
    const u32 descriptor =
        hle::Linker::internal_descriptor(hle::InternalFunction::gcm_command_buffer_full);
    const u32 thunk = memory.read<u32>(descriptor);
    const u32 index = hle::Linker::k_internal_index;
    CHECK(memory.read<u32>(thunk) == (0x04000000 | index));
    CHECK(memory.read<u32>(thunk + 4) == 0x4e800020);
    CHECK(linker.name(index) == "(firmware).cellGcmCallback");
}

TEST_CASE_METHOD(LinkerTest, "imports are named after their HLE function, or their NID") {
    CHECK(linker.name(lwmutex_lock) == "sysPrxForUser.sys_lwmutex_lock");
    CHECK(linker.name(unknown) == "sysPrxForUser.0x12345678");
}

TEST_CASE_METHOD(LinkerTest, "unimplemented imports report success") {
    PpuThread& thread = new_thread();
    thread.cpu.state.gpr[3] = 99;
    call(thread, unknown, {});
    CHECK(result(thread) == k_ok);
}

TEST_CASE_METHOD(LinkerTest, "lightweight mutexes block and hand over like kernel mutexes") {
    PpuThread& a = new_thread();
    PpuThread& b = new_thread();
    memory.write<u32>(k_lwmutex_attr, k_protocol_fifo);
    memory.write<u32>(k_lwmutex_attr + 4, k_mutex_not_recursive);

    call(a, lwmutex_create, {k_lwmutex, k_lwmutex_attr});
    REQUIRE(result(a) == k_ok);
    call(a, lwmutex_lock, {k_lwmutex, 0});
    call(b, lwmutex_lock, {k_lwmutex, 0});
    CHECK(is_waiting(b));

    call(a, lwmutex_unlock, {k_lwmutex});
    CHECK(b.status == ThreadStatus::ready);
    CHECK(result(b) == k_ok);
}

TEST_CASE_METHOD(LinkerTest, "lightweight condition variables") {
    PpuThread& waiter = new_thread();
    PpuThread& signaler = new_thread();
    memory.write<u32>(k_lwmutex_attr, k_protocol_fifo);
    memory.write<u32>(k_lwmutex_attr + 4, k_mutex_not_recursive);
    call(waiter, lwmutex_create, {k_lwmutex, k_lwmutex_attr});
    call(waiter, lwcond_create, {k_lwcond, k_lwmutex, 0});
    REQUIRE(result(waiter) == k_ok);
    CHECK(memory.read<u32>(k_lwcond) == k_lwmutex);

    call(waiter, lwmutex_lock, {k_lwmutex, 0});
    call(waiter, lwcond_wait, {k_lwcond, 0});
    CHECK(is_waiting(waiter));

    call(signaler, lwmutex_lock, {k_lwmutex, 0});
    call(signaler, lwcond_signal, {k_lwcond});
    call(signaler, lwmutex_unlock, {k_lwmutex});
    CHECK(waiter.status == ThreadStatus::ready);
}

TEST_CASE_METHOD(LinkerTest, "a held spinlock makes the call run again later") {
    PpuThread& thread = new_thread();
    memory.write<u32>(k_lwmutex, 1); // held
    thread.cpu.state.pc = 0x10004;   // just after the `hle` instruction
    call(thread, spinlock_lock, {k_lwmutex});
    CHECK(thread.cpu.state.pc == 0x10000);

    memory.write<u32>(k_lwmutex, 0);
    thread.cpu.state.pc = 0x10004;
    call(thread, spinlock_lock, {k_lwmutex});
    CHECK(thread.cpu.state.pc == 0x10004);
    CHECK(memory.read<u32>(k_lwmutex) == 1);
}

TEST_CASE_METHOD(LinkerTest, "thread ID and process exit") {
    PpuThread& thread = new_thread();
    call(thread, thread_get_id, {k_lwmutex});
    CHECK(memory.read<u64>(k_lwmutex) == thread.id);

    call(thread, process_exit, {3});
    CHECK(kernel.exit_status() == 3);
}

TEST_CASE_METHOD(LinkerTest, "an imported SPU image lists copy and fill segments") {
    constexpr u64 elf = KernelTest::k_data + 0x100;
    constexpr u64 image = KernelTest::k_data + 0x40;
    const std::vector<u8> file = test::make_spu_elf({{0x0, {1, 2, 3, 4, 5, 6, 7, 8}, 0x20}}, 0x4);
    memory.write_bytes(elf, file);
    PpuThread& thread = new_thread();

    call(thread, spu_image_import, {image, elf, 0});
    REQUIRE(result(thread) == k_ok);
    CHECK(memory.read<u32>(image + 4) == 0x4); // entry point
    CHECK(memory.read<u32>(image + 12) == 2);  // segment count
    const u32 table = memory.read<u32>(image + 8);
    // {type, local store address, size, source or fill value}
    CHECK(memory.read<u32>(table) == k_spu_segment_copy);
    CHECK(memory.read<u32>(table + 8) == 8);
    CHECK(memory.read<u32>(table + 12) == elf + 52 + 32); // the ELF's own bytes
    CHECK(memory.read<u32>(table + 16) == k_spu_segment_fill);
    CHECK(memory.read<u32>(table + 20) == 8);
    CHECK(memory.read<u32>(table + 24) == 0x18);

    call(thread, spu_image_close, {image});
    CHECK(result(thread) == k_ok);
    CHECK_FALSE(memory.is_accessible(table, 4, Memory::k_read));
}

TEST_CASE_METHOD(LinkerTest, "importing something that isn't an SPU program fails") {
    PpuThread& thread = new_thread();
    memory.write<u32>(KernelTest::k_data + 0x100, 0x1234'5678);
    call(thread, spu_image_import, {KernelTest::k_data + 0x40, KernelTest::k_data + 0x100, 0});
    CHECK(result(thread) == k_error_invalid);
}
