#include "debug/trace_diff.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

using debug::diff_traces;

TEST_CASE("identical traces have no difference") {
    std::istringstream a("--- thread 0x1000 \"main\"\n0x00010000  38600000  li r3, 0\n");
    std::istringstream b("--- thread 0x1000 \"main\"\n0x00010000  38600000  li r3, 0\n");
    CHECK_FALSE(diff_traces(a, b));
}

TEST_CASE("the first differing line is reported with its thread and context") {
    const std::string shared = "--- thread 0x1000 \"main\"\n"
                               "0x00010000  38600001  li r3, 1  -> r3=0x1\n"
                               "    syscall 403 sys_tty_write\n"
                               "--- SPU thread 0x1001 \"worker\"\n"
                               "spu 0x00000  40800083  il $3, 1\n";
    std::istringstream a(shared + "spu 0x00004  1c010183  ai $3, $3, 4  -> $3=5\nsame\n");
    std::istringstream b(shared + "spu 0x00004  1c010183  ai $3, $3, 4  -> $3=6\nsame\n");

    const auto difference = diff_traces(a, b, 2);
    REQUIRE(difference);
    CHECK(difference->line == 6);
    CHECK(difference->instructions == 2);
    CHECK(difference->thread == "SPU thread 0x1001 \"worker\"");
    CHECK(debug::describe(*difference) ==
          "traces differ at line 6, after 2 instructions, in SPU thread 0x1001 \"worker\":\n"
          "  --- SPU thread 0x1001 \"worker\"\n"
          "  spu 0x00000  40800083  il $3, 1\n"
          "- spu 0x00004  1c010183  ai $3, $3, 4  -> $3=5\n"
          "+ spu 0x00004  1c010183  ai $3, $3, 4  -> $3=6\n");
}

TEST_CASE("a trace that stops early differs where it ends") {
    std::istringstream a("0x00010000  38600000  li r3, 0\n0x00010004  38600000  li r3, 0\n");
    std::istringstream b("0x00010000  38600000  li r3, 0\n");
    const auto difference = diff_traces(a, b);
    REQUIRE(difference);
    CHECK(difference->line == 2);
    CHECK(difference->instructions == 1);
    CHECK_FALSE(difference->second);
    CHECK(debug::describe(*difference) == "traces differ at line 2, after 1 instruction:\n"
                                          "  0x00010000  38600000  li r3, 0\n"
                                          "- 0x00010004  38600000  li r3, 0\n"
                                          "+ (end of trace)\n");
}
