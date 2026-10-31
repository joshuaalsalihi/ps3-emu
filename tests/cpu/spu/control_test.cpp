#include "support/spu_test.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

using spu::StopReason;
using test::SpuTest;

TEST_CASE_METHOD(SpuTest, "stop signals its type and leaves pc after the instruction") {
    run({"nop $0", "stop 0x2001", "il $3, 1"});
    REQUIRE(stopped_with(StopReason::stop));
    CHECK(thread.stop->signal == 0x2001);
    CHECK(thread.state.pc == 8);
    CHECK(preferred(3) == 0);
}

TEST_CASE_METHOD(SpuTest, "stopd stops with its own signal type") {
    run({"stopd $0, $0, $0"});
    REQUIRE(stopped_with(StopReason::stop));
    CHECK(thread.stop->signal == spu::k_stopd_signal);
}

TEST_CASE_METHOD(SpuTest, "channel instructions stop with pc on the instruction") {
    for (const auto* line : {"rdch $3, $ch29", "rchcnt $3, $ch29", "wrch $ch28, $3"}) {
        INFO(line);
        run({"nop $0", line});
        CHECK(stopped_with(StopReason::channel));
        CHECK(thread.state.pc == 4);
    }
}

TEST_CASE_METHOD(SpuTest, "illegal instructions fault without advancing") {
    thread.local_store.write<u32>(0, 0x78614203); // dfceq, which the Cell lacks
    spu::interpret(thread, 1);
    REQUIRE(stopped_with(StopReason::fault));
    CHECK(thread.stop->message == "illegal instruction 0x78614203");
    CHECK(thread.state.pc == 0);
}

TEST_CASE_METHOD(SpuTest, "undefined special-purpose registers read as zero") {
    set_all(3, 0x1234'5678);
    run({"mtspr $sp5, $3", "mfspr $3, $sp5"});
    CHECK(r(3) == Vector{});
}

TEST_CASE_METHOD(SpuTest, "no-ops and synchronization change nothing") {
    set_all(127, 0x55);
    run({"lnop", "nop $127", "sync", "syncc", "dsync"});
    CHECK_FALSE(thread.stop);
    CHECK(r(127) == Vector::from_words(0x55, 0x55, 0x55, 0x55));
}

TEST_CASE_METHOD(SpuTest, "instruction fetch wraps around the end of local store") {
    thread.local_store.write<u32>(0x3fffc, spu::assemble("il $3, 1"));
    thread.local_store.write<u32>(0, spu::assemble("ai $3, $3, 6"));
    thread.state.pc = 0x3fffc;
    spu::interpret(thread, 2);
    CHECK(preferred(3) == 7);
    CHECK(thread.state.pc == 4);
}

TEST_CASE_METHOD(SpuTest, "the interpreter traces each instruction and counts cycles") {
    thread.local_store.write<u32>(0, spu::assemble("il $3, 1"));
    thread.local_store.write<u32>(4, spu::assemble("br 0", 4));
    std::ostringstream trace;
    CHECK(spu::interpret(thread, 3, &trace) == 3);
    CHECK(clock.cycles() == 3);
    CHECK(trace.str() == "spu 0x00000  40800083  il $3, 1\n"
                         "spu 0x00004  327fff80  br 0x0\n"
                         "spu 0x00000  40800083  il $3, 1\n");
}
