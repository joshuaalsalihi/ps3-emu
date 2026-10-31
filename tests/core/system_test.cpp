// Whole programs, run through System: loading, scheduling, syscalls and HLE imports.

#include "core/system.h"

#include "cpu/ppu/assembler.h"
#include "hle/nid.h"
#include "support/elf_builder.h"
#include "support/import_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sstream>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

constexpr u64 k_code = 0x10000;
constexpr u64 k_data = 0x20000;
constexpr u64 k_data_size = 0x400;

constexpr u32 k_read_execute = loader::k_segment_flag_r | loader::k_segment_flag_x;
constexpr u32 k_read_write = loader::k_segment_flag_r | loader::k_segment_flag_w;

// A program with `code` at 0x10000 and `data` at 0x20000. The entry descriptor is the data's
// first 8 bytes, so data[0..7] must be {0x10000, 0}.
std::vector<u8> make_test_program(const std::vector<std::string_view>& code, std::vector<u8> data,
                                  const std::vector<test::InfoSegmentSpec>& info = {}) {
    return test::make_program(
        {
            {k_code, k_read_execute, test::words_to_bytes(ppu::assemble(code, k_code))},
            {k_data, k_read_write, std::move(data)},
        },
        k_data, info);
}

// Data with the entry descriptor followed by "hi\n" at 0x20008.
std::vector<u8> hello_data() {
    std::vector<u8> data = test::words_to_bytes({k_code, 0});
    data.insert(data.end(), {'h', 'i', '\n'});
    return data;
}

const std::vector<std::string_view> k_hello_and_exit = {
    "li r3, 0", "lis r4, 2", "addi r4, r4, 8",
    "li r5, 3", "li r6, 0",  "li r11, 403",
    "sc",                          // sys_tty_write
    "li r3, 7", "li r11, 3", "sc", // sys_process_exit
};

} // namespace

TEST_CASE("a program prints and exits") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_hello_and_exit, hello_data()));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 7);
    CHECK(result.instructions == 10);
    CHECK(tty.str() == "hi\n");
}

TEST_CASE("the main thread starts with a stack and the TOC") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program({"nop"}, hello_data()));

    const lv2::PpuThread& main = system.main_thread();
    const ppu::State& state = main.cpu.state;
    CHECK(main.name == "main");
    CHECK(state.pc == k_code);
    CHECK(state.gpr[1] > main.stack_address);
    CHECK(state.gpr[1] < main.stack_address + main.stack_size);
    CHECK(state.gpr[1] % 16 == 0);
    CHECK(state.gpr[2] == 0); // the descriptor's TOC
    CHECK(main.stack_size == System::k_default_stack_size);
}

TEST_CASE("returning from the entry point ends the process") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program({"li r3, 5", "blr"}, hello_data()));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 5);
}

TEST_CASE("a fault stops the run and names the thread and instruction") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program({"li r4, 0", "lwz r3, 0(r4)"}, hello_data()));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::faulted);
    CHECK_THAT(result.message, ContainsSubstring("\"main\""));
    CHECK_THAT(result.message, ContainsSubstring("invalid memory read at 0x0"));
    CHECK_THAT(result.message, ContainsSubstring("pc 0x10004: lwz r3, 0(r4)"));
}

TEST_CASE("runs can be limited and traced") {
    std::ostringstream tty;
    std::ostringstream trace;
    System system(tty);
    system.load(make_test_program({"li r3, 1", "b 0x10000"}, hello_data()));

    const RunResult result = system.run({.max_instructions = 5, .trace = &trace});
    CHECK(result.outcome == RunResult::Outcome::instruction_limit);
    CHECK(result.instructions == 5);
    CHECK_THAT(trace.str(), StartsWith("--- thread 0x1000 \"main\"\n"
                                       "0x00010000  38600001  li r3, 1\n"
                                       "0x00010004  4bfffffc  b 0x10000\n"));
}

TEST_CASE("threads run concurrently and join") {
    // Data: main's descriptor, the worker's descriptor, a thread parameter block, then space
    // for the new thread's ID (0x20020) and its exit value (0x20028).
    std::vector<u8> data = test::words_to_bytes({
        k_code,
        0, // 0x20000: main's descriptor
        k_code + 0x5c,
        0, // 0x20008: the worker's descriptor
        k_data + 0x08,
        0, // 0x20010: thread parameters {entry descriptor, TLS}
        0,
        0,
        0,
        0, // 0x20018..0x20027
        0,
        0, // 0x20028: exit value
    });

    const std::vector<std::string_view> code = {
        // sys_ppu_thread_create(&id, &param, 0, 0, 1000, 0x10000, joinable, no name)
        "lis r3, 2",
        "addi r3, r3, 0x20",
        "lis r4, 2",
        "addi r4, r4, 0x10",
        "li r5, 0",
        "li r6, 0",
        "li r7, 1000",
        "lis r8, 1",
        "li r9, 1",
        "li r10, 0",
        "li r11, 52",
        "sc",
        // sys_ppu_thread_start(id)
        "lis r31, 2",
        "ld r3, 0x20(r31)",
        "li r11, 53",
        "sc",
        // sys_ppu_thread_join(id, &exit_value)
        "ld r3, 0x20(r31)",
        "addi r4, r31, 0x28",
        "li r11, 44",
        "sc",
        // sys_process_exit(exit_value)
        "lwz r3, 0x2c(r31)",
        "li r11, 3",
        "sc",
        // The worker, at 0x1005c: sys_ppu_thread_exit(7)
        "li r3, 7",
        "li r11, 41",
        "sc",
    };

    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(code, data));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 7);
    CHECK(system.kernel().threads().size() == 2);
}

TEST_CASE("a program whose threads all wait forever is deadlocked") {
    // sys_semaphore_create(&id, &attr, 0, 1), then sys_semaphore_wait(id, no timeout).
    std::vector<u8> data = test::words_to_bytes({k_code, 0, 1 /* FIFO */, 0, 0, 0});
    const std::vector<std::string_view> code = {
        "lis r31, 2",
        "addi r3, r31, 0x14",
        "addi r4, r31, 0x8",
        "li r5, 0",
        "li r6, 1",
        "li r11, 90",
        "sc",
        "lwz r3, 0x14(r31)",
        "li r4, 0",
        "li r11, 92",
        "sc",
    };

    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(code, data));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::deadlocked);
    CHECK_THAT(result.message, ContainsSubstring("\"main\" waits on semaphore"));
}

TEST_CASE("sleeping moves the virtual clock forward") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program({"li r3, 1000", "li r11, 141", "sc", // usleep(1000)
                                   "li r3, 0", "li r11, 3", "sc"},
                                  hello_data()));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(system.clock().cycles() >= 1000 * 3200);
}

TEST_CASE("imported functions run their HLE implementation") {
    // Import tables for sys_process_exit, built in scratch memory and copied into the data.
    Memory scratch;
    scratch.map(k_data, Memory::k_page_size, Memory::k_read_write);
    const auto tables = test::write_import_tables(scratch, k_data + 0x100, "sysPrxForUser",
                                                  {hle::nid("sys_process_exit")});
    std::vector<u8> data(k_data_size);
    scratch.read_bytes(k_data, data);
    const std::vector<u8> descriptor = test::words_to_bytes({k_code, 0});
    std::copy(descriptor.begin(), descriptor.end(), data.begin());
    REQUIRE(tables.stub_slots[0] == k_data + 0x240);

    // Call the import as PSL1GHT's stubs do: load the descriptor from the stub slot, set the
    // TOC, and branch to the code address.
    const std::vector<std::string_view> code = {
        "lis r12, 2", "lwz r12, 0x240(r12)", "lwz r0, 0(r12)", "lwz r2, 4(r12)",
        "mtctr r0",   "li r3, 42",           "bctrl",
    };

    std::ostringstream tty;
    std::ostringstream trace;
    System system(tty);
    system.load(
        make_test_program(code, data, {{loader::k_segment_prx_param, tables.prx_param, 0x28}}));

    const RunResult result = system.run({.trace = &trace});
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 42);
    CHECK_THAT(trace.str(), ContainsSubstring("call sysPrxForUser.sys_process_exit"));
}

TEST_CASE("runs are deterministic") {
    auto run_once = [] {
        std::ostringstream tty;
        System system(tty);
        system.load(make_test_program(k_hello_and_exit, hello_data()));
        system.run();
        return std::make_pair(system.main_thread().cpu.state.gpr, system.clock().cycles());
    };
    CHECK(run_once() == run_once());
}
