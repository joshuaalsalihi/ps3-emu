// Branch, condition register, special register, trap and system instructions.
// Programs start at 0x10000 (PpuTest::k_code); branch targets are absolute addresses.

#include "support/ppu_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace ppu;
using test::PpuTest;

// ---- Branch ----

TEST_CASE_METHOD(PpuTest, "b skips forward, bl also links") {
    run({"bl 0x10008", "li r3, 1", "li r4, 2"});
    CHECK(r(3) == 0);
    CHECK(r(4) == 2);
    CHECK(state().lr == 0x10004);
}

TEST_CASE_METHOD(PpuTest, "conditional branches test one CR bit") {
    r(3) = 5;
    SECTION("taken") {
        run({"cmpwi r3, 5", "beq 0x1000c", "li r4, 1", "li r5, 1"});
        CHECK(r(4) == 0);
        CHECK(r(5) == 1);
    }
    SECTION("not taken") {
        run({"cmpwi r3, 6", "beq 0x1000c", "li r4, 1", "li r5, 1"});
        CHECK(r(4) == 1);
    }
    SECTION("other CR fields") {
        run({"cmpwi cr7, r3, 9", "blt cr7, 0x1000c", "li r4, 1", "li r5, 1"});
        CHECK(r(4) == 0);
    }
}

TEST_CASE_METHOD(PpuTest, "bdnz loops until CTR reaches zero") {
    run({
        "li r3, 0",
        "li r4, 5",
        "mtctr r4",
        "addi r3, r3, 2", // 0x1000c
        "bdnz 0x1000c",
    });
    CHECK(r(3) == 10);
    CHECK(state().ctr == 0);
}

TEST_CASE_METHOD(PpuTest, "bdz branches when CTR becomes zero") {
    state().ctr = 1;
    run({"bdz 0x10008", "li r3, 1", "li r4, 1"});
    CHECK(r(3) == 0);
    CHECK(state().ctr == 0);
}

TEST_CASE_METHOD(PpuTest, "blrl jumps to the old LR and saves the new one") {
    state().lr = 0x10009; // the low two bits of the target are ignored
    run({"blrl", "li r3, 1", "li r4, 2"});
    CHECK(r(3) == 0);
    CHECK(r(4) == 2);
    CHECK(state().lr == 0x10004);
}

TEST_CASE_METHOD(PpuTest, "bctr and bcctrl jump to CTR") {
    state().ctr = 0x1000c;
    run({"bctrl", "li r3, 1", "li r4, 1", "li r5, 1"});
    CHECK(r(3) == 0);
    CHECK(r(4) == 0);
    CHECK(r(5) == 1);
    CHECK(state().lr == 0x10004);
}

TEST_CASE_METHOD(PpuTest, "conditional bclr") {
    state().lr = 0x1000c;
    state().cr = 0x20000000; // CR0[EQ]
    run({"bclr 12, 2", "li r3, 1", "li r4, 1", "li r5, 1"});
    CHECK(r(3) == 0);
    CHECK(r(5) == 1);
}

// ---- Condition register ----

TEST_CASE_METHOD(PpuTest, "CR logical operations") {
    state().set_cr_bit(1, true);
    state().set_cr_bit(2, false);
    run({"crand 8, 1, 2", "cror 9, 1, 2", "crxor 10, 1, 1", "crnand 11, 1, 2", "crnor 12, 1, 2",
         "creqv 13, 2, 2", "crandc 14, 1, 2", "crorc 15, 2, 1"});
    CHECK_FALSE(state().cr_bit(8));
    CHECK(state().cr_bit(9));
    CHECK_FALSE(state().cr_bit(10));
    CHECK(state().cr_bit(11));
    CHECK_FALSE(state().cr_bit(12));
    CHECK(state().cr_bit(13));
    CHECK(state().cr_bit(14));
    CHECK_FALSE(state().cr_bit(15));
}

TEST_CASE_METHOD(PpuTest, "mcrf, mfcr and mtcrf") {
    state().cr = 0x12345678;
    r(4) = 0xabcdef01;
    run({"mcrf cr0, cr7", "mfcr r3", "mtcrf 0x81, r4"});
    CHECK(r(3) == 0x82345678);
    // mtcrf 0x81 replaces only CR0 and CR7.
    CHECK(state().cr == 0xa2345671);
}

// ---- Special registers ----

TEST_CASE_METHOD(PpuTest, "moving to and from special registers") {
    r(3) = 0xe000007f; // SO, OV, CA and a byte count
    r(4) = 0x1234;
    run({"mtspr xer, r3", "mtlr r4", "mtctr r4", "mfspr r5, xer", "mflr r6", "mfctr r7",
         "mtspr vrsave, r4", "mfspr r8, vrsave"});
    CHECK(xer().so);
    CHECK(xer().ov);
    CHECK(xer().ca);
    CHECK(r(5) == 0xe000007f);
    CHECK(r(6) == 0x1234);
    CHECK(r(7) == 0x1234);
    CHECK(r(8) == 0x1234);
}

TEST_CASE_METHOD(PpuTest, "the time base comes from the virtual clock") {
    clock.advance(16'000 * 0x1'0000'0000);
    run({"mftb r3, tb", "mftb r4, tbu", "mfspr r5, tb"});
    // 16000 CPU cycles are 399 time base ticks.
    CHECK(r(3) == 399 * 0x1'0000'0000);
    CHECK(r(4) == 399);
    CHECK(r(5) == 399 * 0x1'0000'0000); // the clock moved by 2 cycles: less than one tick
}

TEST_CASE_METHOD(PpuTest, "unsupported special registers fault") {
    run({"mfspr r3, 1013"});
    CHECK(stopped_with(StopReason::fault));
    CHECK(state().pc == k_code);
}

// ---- Traps and system calls ----

TEST_CASE_METHOD(PpuTest, "tw 31 always traps and stays on the instruction") {
    run({"li r3, 1", "tw 31, r0, r0", "li r4, 1"});
    CHECK(stopped_with(StopReason::trap));
    CHECK(state().pc == k_code + 4);
    CHECK(r(4) == 0);
}

TEST_CASE_METHOD(PpuTest, "trap conditions") {
    r(3) = static_cast<u64>(-1);
    r(4) = 1;
    SECTION("signed less than") {
        run({"tw 16, r3, r4"});
        CHECK(stopped_with(StopReason::trap));
    }
    SECTION("unsigned greater than") {
        run({"td 1, r3, r4"});
        CHECK(stopped_with(StopReason::trap));
    }
    SECTION("equal, not met") {
        run({"twi 4, r3, 0", "tdi 4, r4, 0"});
        CHECK_FALSE(thread.stop);
    }
}

TEST_CASE_METHOD(PpuTest, "sc stops for the kernel and resumes after it") {
    run({"sc", "li r3, 1"});
    CHECK(stopped_with(StopReason::syscall));
    CHECK(state().pc == k_code + 4);
}

TEST_CASE_METHOD(PpuTest, "barriers do nothing") {
    run({"sync", "isync", "eieio", "li r3, 1"});
    CHECK(r(3) == 1);
    CHECK_FALSE(thread.stop);
}
