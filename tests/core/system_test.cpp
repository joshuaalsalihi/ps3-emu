#include "core/system.h"

#include "cpu/ppu/assembler.h"
#include "support/elf_builder.h"

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

// A program with `code` at 0x10000 and a data page at 0x20000 holding the entry descriptor,
// followed by the bytes of "hi\n" at 0x20008.
std::vector<u8> make_test_program(const std::vector<std::string_view>& code) {
    std::vector<u8> data = test::words_to_bytes({k_code, 0});
    data.insert(data.end(), {'h', 'i', '\n'});

    return test::make_program(
        {
            {k_code, loader::k_segment_flag_r | loader::k_segment_flag_x,
             test::words_to_bytes(ppu::assemble(code, k_code))},
            {k_data, loader::k_segment_flag_r | loader::k_segment_flag_w, data},
        },
        k_data);
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
    system.load(make_test_program(k_hello_and_exit));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::exited);
    CHECK(result.exit_status == 7);
    CHECK(result.instructions == 10);
    CHECK(tty.str() == "hi\n");
}

TEST_CASE("the main thread starts with a stack and the TOC") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program({"nop"}));

    const ppu::State& state = system.main_thread().state;
    CHECK(state.pc == k_code);
    CHECK(state.gpr[1] > System::k_stack_base);
    CHECK(state.gpr[1] < System::k_stack_base + System::k_stack_size);
    CHECK(state.gpr[1] % 16 == 0);
    CHECK(state.gpr[2] == 0); // the descriptor's TOC
}

TEST_CASE("a fault stops the run and names the instruction") {
    std::ostringstream tty;
    System system(tty);
    system.load(make_test_program({"li r4, 0", "lwz r3, 0(r4)"}));

    const RunResult result = system.run();
    CHECK(result.outcome == RunResult::Outcome::faulted);
    CHECK_THAT(result.message, ContainsSubstring("invalid memory read at 0x0"));
    CHECK_THAT(result.message, ContainsSubstring("pc 0x10004: lwz r3, 0(r4)"));
}

TEST_CASE("runs can be limited and traced") {
    std::ostringstream tty;
    std::ostringstream trace;
    System system(tty);
    system.load(make_test_program({"li r3, 1", "b 0x10000"}));

    const RunResult result = system.run({.max_instructions = 5, .trace = &trace});
    CHECK(result.outcome == RunResult::Outcome::instruction_limit);
    CHECK(result.instructions == 5);
    CHECK_THAT(trace.str(), StartsWith("0x00010000  38600001  li r3, 1\n"
                                       "0x00010004  4bfffffc  b 0x10000\n"));
}

TEST_CASE("runs are deterministic") {
    auto run_once = [] {
        std::ostringstream tty;
        System system(tty);
        system.load(make_test_program(k_hello_and_exit));
        system.run();
        return std::make_pair(system.main_thread().state.gpr, system.clock().cycles());
    };
    CHECK(run_once() == run_once());
}
