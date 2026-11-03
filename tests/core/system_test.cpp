// Whole programs, run through System: loading, scheduling, syscalls and HLE imports.

#include "core/system.h"

#include "cpu/ppu/assembler.h"
#include "cpu/spu/assembler.h"
#include "hle/nid.h"
#include "support/elf_builder.h"
#include "support/import_builder.h"
#include "support/test_programs.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace test;

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
    // Each instruction shows the registers it changed; r3 is only new the first time.
    CHECK_THAT(trace.str(), StartsWith("--- thread 0x1000 \"main\"\n"
                                       "0x00010000  38600001  li r3, 1  -> r3=0x1\n"
                                       "0x00010004  4bfffffc  b 0x10000\n"
                                       "0x00010000  38600001  li r3, 1\n"));
}

TEST_CASE("the trace shows every kind of register an instruction changes") {
    std::ostringstream tty;
    std::ostringstream trace;
    System system(tty);
    system.load(make_test_program(
        {"li r3, -1", "mtctr r3", "cmpwi r3, 0", "addic r4, r3, 1", "vspltisw v2, 1", "mtlr r3"},
        hello_data()));
    system.run({.max_instructions = 6, .trace = &trace});
    CHECK_THAT(trace.str(), ContainsSubstring("mtctr r3  -> ctr=0xffffffffffffffff\n"));
    CHECK_THAT(trace.str(), ContainsSubstring("cmpwi r3, 0  -> cr=0x80000000\n"));
    CHECK_THAT(trace.str(), ContainsSubstring("addic r4, r3, 1  -> xer=0x20000000\n"));
    CHECK_THAT(trace.str(),
               ContainsSubstring("vspltisw v2, 1  -> v2=00000001000000010000000100000001\n"));
    CHECK_THAT(trace.str(), ContainsSubstring("mtlr r3  -> lr=0xffffffffffffffff\n"));
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

// ---- SPU threads ----

TEST_CASE("a PPU thread runs an SPU thread and collects its result") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data(k_spu_add_one)));

    const RunResult result = system.run();
    INFO(result.message);
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 43);
    CHECK(system.memory().read<u32>(k_data + 0x88) == lv2::k_spu_join_all_threads_exit);
    CHECK(system.kernel().spu_threads().front()->exit_status == 43);
}

TEST_CASE("SPU runs are deterministic, with or without instant DMA") {
    auto run_once = [](bool instant) {
        std::ostringstream tty;
        System system(tty);
        system.load(make_test_program(k_run_spu_thread, spu_program_data(k_spu_add_one)));
        const RunResult result = system.run({.instant_dma = instant});
        return std::make_pair(result.exit_status, system.clock().cycles());
    };
    CHECK(run_once(false) == run_once(false));
    CHECK(run_once(true).first == run_once(false).first);
    CHECK(run_once(true).second < run_once(false).second); // no waiting for the DMA
}

TEST_CASE("an SPU thread waiting on its mailbox forever is a deadlock") {
    // The same program, but the SPU reads the mailbox twice.
    std::vector<std::string_view> spu_code = k_spu_add_one;
    spu_code.insert(spu_code.begin(), "rdch $4, $ch29");
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data(spu_code)));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::deadlocked);
    CHECK_THAT(result.message, ContainsSubstring("waits on SPU thread group"));
    CHECK_THAT(result.message, ContainsSubstring("waits on channel 29 (SPU_RdInMbox)"));
}

TEST_CASE("an SPU fault ends the run with the SPU's instruction") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data({"heqi $0, 0"})));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::faulted);
    CHECK_THAT(result.message, StartsWith("SPU thread 0x"));
    CHECK_THAT(result.message, ContainsSubstring("halt instruction (pc 0x0: heqi $0, 0)"));
}

TEST_CASE("an SPU stop that LV2 doesn't know is a fault") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data({"stop 0x1234"})));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::faulted);
    CHECK_THAT(result.message,
               ContainsSubstring("stop 0x1234, which LV2 gives no meaning (pc 0x0: stop 0x1234)"));
}

TEST_CASE("an SPU program calling exit() is told to use spu_thread_exit") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data({"stop 0x2005"})));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::faulted);
    CHECK_THAT(result.message, ContainsSubstring("stop 0x2005: exit(5) from the SPU C runtime"));
    CHECK_THAT(result.message, ContainsSubstring("use spu_thread_exit instead"));
}

// ---- Save states ----

namespace {

// Gets 16 bytes (starting with the nonzero entry descriptor) by DMA, then counts how many times
// it reads them from the local store before they arrive. Exits with the count. No channel
// access comes between the reads, so only the DMA's timing decides when the data appears.
const std::vector<std::string_view> k_spu_poll_dma = {
    "il $10, 0x200",   "ilhu $11, 0x2",   "il $12, 16",      "il $14, 0x40", "wrch $ch16, $10",
    "wrch $ch18, $11", "wrch $ch19, $12", "wrch $ch21, $14", "il $6, 0",     "lqa $5, 0x200",
    "ai $6, $6, 1",    "brz $5, 0x24",    "wrch $ch28, $6",  "stop 0x102",
};

// Runs a program with one SPU thread to the end, stopping first after `split` instructions if
// it is set. Returns the guest's output and the final state.
std::pair<std::string, std::vector<u8>>
run_spu_program(std::optional<u64> split,
                const std::vector<std::string_view>& spu_code = k_spu_add_one) {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data(spu_code)));
    if (split)
        REQUIRE(system.run({.max_instructions = *split}).outcome ==
                RunResult::Outcome::instruction_limit);
    const RunResult result = system.run();
    REQUIRE(result.outcome == RunResult::Outcome::exited);
    return {tty.str(), system.save_state()};
}

} // namespace

TEST_CASE("stopping a run anywhere and continuing changes nothing") {
    const auto [output, final_state] = run_spu_program(std::nullopt);
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data(k_spu_add_one)));
    const u64 total = system.run().instructions;
    REQUIRE(total > 50);

    // Every split point, including those in the middle of SPU turns and DMA transfers.
    for (u64 split = 1; split < total; ++split) {
        INFO("split after " << split << " instructions");
        CHECK(run_spu_program(split).second == final_state);
    }
}

TEST_CASE("DMA lands at the same instruction however a turn is split") {
    // The SPU used to bring DMA up to date only between batches of instructions, so data
    // appeared later when a run stopped in the middle of a turn.
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data(k_spu_poll_dma)));
    const RunResult result = system.run();
    REQUIRE(result.outcome == RunResult::Outcome::exited);
    const s32 polls = system.kernel().spu_threads().front()->exit_status.value();
    // The transfer takes 258 cycles (256 to set up, 16 bytes at 8 a cycle). The first read is
    // one cycle after the command, and each poll takes 3, so the k-th poll sees the data once
    // 1 + 3 (k - 1) >= 258: k = 87.
    CHECK(polls == 87);

    const auto [output, final_state] = run_spu_program(std::nullopt, k_spu_poll_dma);
    for (u64 split = 1; split < result.instructions; split += 7) {
        INFO("split after " << split << " instructions");
        CHECK(run_spu_program(split, k_spu_poll_dma).second == final_state);
    }
}

TEST_CASE("a saved state continues in a new System as in the original") {
    const auto [output, final_state] = run_spu_program(std::nullopt);

    for (const u64 split : {1, 15, 30, 50}) {
        INFO("saved after " << split << " instructions");
        std::ostringstream tty;
        System original(tty);
        original.load(make_test_program(k_run_spu_thread, spu_program_data(k_spu_add_one)));
        original.run({.max_instructions = split});
        const std::vector<u8> saved = original.save_state();

        System restored(tty);
        restored.load_state(saved);
        CHECK(restored.instructions() == split);
        CHECK(restored.save_state() == saved);
        const RunResult result = restored.run();
        CHECK(result.outcome == RunResult::Outcome::exited);
        CHECK(result.exit_status == 43);
        CHECK(restored.save_state() == final_state);
    }
}

TEST_CASE("a state that can't be loaded is rejected") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_hello_and_exit, hello_data()));
    std::vector<u8> saved = system.save_state();

    SECTION("not a state") {
        saved[0] = 'X';
        System fresh(tty);
        CHECK_THROWS_WITH(fresh.load_state(saved), "not a save state");
    }
    SECTION("another version") {
        saved[8] = 99;
        System fresh(tty);
        CHECK_THROWS_WITH(fresh.load_state(saved),
                          "save state is version 99, but this build reads version 2");
    }
    SECTION("truncated") {
        saved.resize(saved.size() / 2);
        System fresh(tty);
        CHECK_THROWS_AS(fresh.load_state(saved), state::Error);
    }
    SECTION("extra bytes") {
        saved.push_back(0);
        System fresh(tty);
        CHECK_THROWS_WITH(fresh.load_state(saved), "save state has extra bytes at the end");
    }
}

TEST_CASE("loading a state replaces what a System was running") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_run_spu_thread, spu_program_data(k_spu_add_one)));
    system.run({.max_instructions = 20});
    const std::vector<u8> saved = system.save_state();

    // Run on to the end, then go back.
    REQUIRE(system.run().outcome == RunResult::Outcome::exited);
    const std::vector<u8> finished = system.save_state();
    system.load_state(saved);
    CHECK(system.save_state() == saved);
    CHECK(system.instructions() == 20);
    REQUIRE(system.run().outcome == RunResult::Outcome::exited);
    CHECK(system.save_state() == finished);
}

// ---- Debugging ----

TEST_CASE("a breakpoint stops a thread before the instruction, and stepping moves past it") {
    std::ostringstream reference_tty;
    System reference(reference_tty);
    reference.load(make_test_program(k_hello_and_exit, hello_data()));
    reference.run();

    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program(k_hello_and_exit, hello_data()));
    const std::set<u64> breakpoints = {k_code + 8, k_code + 24}; // addi, then sc
    const u32 main = system.main_thread().id;

    RunResult result = system.run({.breakpoints = &breakpoints});
    CHECK(result.outcome == RunResult::Outcome::breakpoint);
    CHECK(result.thread == main);
    CHECK(result.instructions == 2);
    CHECK(system.main_thread().cpu.state.pc == k_code + 8);

    // Running again with the breakpoint still there makes no progress.
    result = system.run({.breakpoints = &breakpoints});
    CHECK(result.outcome == RunResult::Outcome::breakpoint);
    CHECK(result.instructions == 0);

    result = system.run({.step_thread = main});
    CHECK(result.outcome == RunResult::Outcome::stepped);
    CHECK(result.instructions == 1);
    CHECK(system.main_thread().cpu.state.pc == k_code + 12);

    // On to the syscall, then step over it: the kernel handles it within the step.
    result = system.run({.breakpoints = &breakpoints});
    CHECK(system.main_thread().cpu.state.pc == k_code + 24);
    CHECK(tty.str().empty());
    result = system.run({.step_thread = main});
    CHECK(result.outcome == RunResult::Outcome::stepped);
    CHECK(tty.str() == "hi\n");

    result = system.run({.breakpoints = &breakpoints});
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 7);
    CHECK(system.save_state() == reference.save_state());
}
