// Load, store, atomic and cache instructions. Data lives at 0x20000 (PpuTest::k_data).

#include "support/ppu_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace ppu;
using test::PpuTest;

TEST_CASE_METHOD(PpuTest, "loads zero-extend, algebraic loads sign-extend") {
    memory.write<u64>(k_data, 0x8182838485868788);
    r(4) = k_data;
    run({"lbz r10, 0(r4)", "lhz r11, 0(r4)", "lwz r12, 0(r4)", "ld r13, 0(r4)", "lha r14, 0(r4)",
         "lwa r15, 0(r4)"});
    CHECK(r(10) == 0x81);
    CHECK(r(11) == 0x8182);
    CHECK(r(12) == 0x81828384);
    CHECK(r(13) == 0x8182838485868788);
    CHECK(r(14) == 0xffffffffffff8182);
    CHECK(r(15) == 0xffffffff81828384);
}

TEST_CASE_METHOD(PpuTest, "indexed loads, with r0 read as zero") {
    memory.write<u32>(k_data + 8, 0xcafef00d);
    r(0) = 0x1000; // ignored when used as rA
    r(4) = k_data;
    r(5) = 8;
    r(6) = k_data + 8;
    run({"lwzx r10, r4, r5", "lwzx r11, r0, r6", "lbzx r12, r4, r5", "lhax r13, r4, r5",
         "lwax r14, r4, r5", "ldx r15, r4, r5", "lhzx r16, r4, r5"});
    CHECK(r(10) == 0xcafef00d);
    CHECK(r(11) == 0xcafef00d);
    CHECK(r(12) == 0xca);
    CHECK(r(13) == 0xffffffffffffcafe);
    CHECK(r(14) == 0xffffffffcafef00d);
    CHECK(r(15) == 0xcafef00d00000000);
    CHECK(r(16) == 0xcafe);
}

TEST_CASE_METHOD(PpuTest, "update forms write the address back to rA") {
    memory.write<u64>(k_data + 16, 0x1122334455667788);
    r(5) = 4;

    SECTION("D-form") {
        r(4) = k_data + 8;
        run({"lwzu r3, 8(r4)"});
        CHECK(r(3) == 0x11223344);
        CHECK(r(4) == k_data + 16);
    }
    SECTION("DS-form") {
        r(4) = k_data;
        run({"ldu r3, 16(r4)"});
        CHECK(r(3) == 0x1122334455667788);
        CHECK(r(4) == k_data + 16);
    }
    SECTION("X-form") {
        r(4) = k_data + 16;
        run({"lhzux r3, r4, r5", "lbzux r6, r4, r5"});
        CHECK(r(3) == 0x5566);
        CHECK(r(6) == 0x00); // k_data + 24 was never written
        CHECK(r(4) == k_data + 24);
    }
    SECTION("all the others") {
        r(4) = k_data + 16;
        r(6) = k_data + 16;
        r(7) = k_data + 16;
        r(8) = k_data + 16;
        r(9) = k_data + 16;
        r(10) = k_data + 16;
        run({"lbzu r11, 1(r4)", "lhau r12, 2(r6)", "lhaux r13, r7, r5", "lwzux r14, r8, r5",
             "lwaux r15, r9, r5", "ldux r16, r10, r5"});
        CHECK(r(11) == 0x22);
        CHECK(r(12) == 0x3344);
        CHECK(r(13) == 0x5566);
        CHECK(r(14) == 0x55667788);
        CHECK(r(15) == 0x55667788);
        CHECK(r(4) == k_data + 17);
        CHECK(r(6) == k_data + 18);
        CHECK(r(7) == k_data + 20);
        CHECK(r(10) == k_data + 20);
    }
}

TEST_CASE_METHOD(PpuTest, "stores write big-endian") {
    r(3) = 0x1122334455667788;
    r(4) = k_data;
    run({"stb r3, 0(r4)", "sth r3, 2(r4)", "stw r3, 4(r4)", "std r3, 8(r4)"});
    CHECK(memory.read<u8>(k_data) == 0x88);
    CHECK(memory.read<u16>(k_data + 2) == 0x7788);
    CHECK(memory.read<u32>(k_data + 4) == 0x55667788);
    CHECK(memory.read<u64>(k_data + 8) == 0x1122334455667788);
}

TEST_CASE_METHOD(PpuTest, "indexed and update stores") {
    r(3) = 0x1122334455667788;
    r(4) = k_data;
    r(5) = 8;
    r(6) = k_data + 0x100;
    run({"stbx r3, r4, r5", "sthx r3, r4, r5", "stwx r3, r4, r5", "stdx r3, r4, r5",
         "stbu r3, 1(r6)", "sthu r3, 2(r6)", "stwu r3, 4(r6)", "stdu r3, 8(r6)", "stbux r3, r6, r5",
         "sthux r3, r6, r5", "stwux r3, r6, r5", "stdux r3, r6, r5"});
    CHECK(memory.read<u64>(k_data + 8) == 0x1122334455667788);
    CHECK(memory.read<u64>(k_data + 0x10f) == 0x1122334455667788);
    CHECK(r(6) == k_data + 0x10f + 32);
    CHECK(memory.read<u64>(r(6)) == 0x1122334455667788);
}

TEST_CASE_METHOD(PpuTest, "byte-reversed loads and stores") {
    memory.write<u32>(k_data, 0x11223344);
    r(4) = k_data;
    r(5) = 0;
    r(6) = 0xaabbccdd;
    r(7) = 8;
    run({"lwbrx r10, r4, r5", "lhbrx r11, r4, r5", "stwbrx r6, r4, r7", "sthbrx r6, r4, r5"});
    CHECK(r(10) == 0x44332211);
    CHECK(r(11) == 0x2211);
    CHECK(memory.read<u32>(k_data + 8) == 0xddccbbaa);
    CHECK(memory.read<u16>(k_data) == 0xddcc);
}

TEST_CASE_METHOD(PpuTest, "lmw and stmw move words to and from r[n]-r31") {
    r(29) = 0xffffffff00000001; // only the low word is stored
    r(30) = 2;
    r(31) = 3;
    r(4) = k_data;
    run({"stmw r29, 0(r4)", "li r29, 0", "li r30, 0", "li r31, 0", "lmw r29, 0(r4)"});
    CHECK(memory.read<u32>(k_data) == 1);
    CHECK(r(29) == 1);
    CHECK(r(30) == 2);
    CHECK(r(31) == 3);
}

// ---- Atomics ----

TEST_CASE_METHOD(PpuTest, "lwarx and stwcx. succeed without interference") {
    memory.write<u32>(k_data, 41);
    r(4) = k_data;
    run({"lwarx r3, r0, r4", "addi r3, r3, 1", "stwcx. r3, r0, r4"});
    CHECK(memory.read<u32>(k_data) == 42);
    CHECK(cr_field(0) == k_cr_eq);
}

TEST_CASE_METHOD(PpuTest, "a store to the reserved line makes stwcx. fail") {
    memory.write<u32>(k_data, 41);
    r(4) = k_data;
    r(5) = 7;
    // The intervening store hits a different word in the same 128-byte line.
    run({"lwarx r3, r0, r4", "stw r5, 64(r4)", "stwcx. r3, r0, r4"});
    CHECK(cr_field(0) == 0);
    CHECK(memory.read<u32>(k_data) == 41);
}

TEST_CASE_METHOD(PpuTest, "a store to another line keeps the reservation") {
    r(4) = k_data;
    r(5) = 7;
    run({"ldarx r3, r0, r4", "std r5, 128(r4)", "stdcx. r5, r0, r4"});
    CHECK(cr_field(0) == k_cr_eq);
    CHECK(memory.read<u64>(k_data) == 7);
}

TEST_CASE_METHOD(PpuTest, "store-conditional fails without a reservation, and uses it up") {
    r(4) = k_data;
    r(5) = 7;
    run({"lwarx r3, r0, r4", "stwcx. r5, r0, r4", "stwcx. r5, r0, r4"});
    CHECK(cr_field(0) == 0);

    xer().so = true;
    run({"stdcx. r5, r0, r4"});
    CHECK(cr_field(0) == k_cr_so);
}

TEST_CASE_METHOD(PpuTest, "unaligned atomics fault") {
    r(4) = k_data + 2;
    run({"lwarx r3, r0, r4"});
    CHECK(stopped_with(StopReason::fault));
}

// ---- Cache ----

TEST_CASE_METHOD(PpuTest, "dcbz zeroes the whole 128-byte line") {
    for (u64 offset = 0; offset < 256; offset += 8) {
        memory.write<u64>(k_data + offset, ~u64{0});
    }
    r(4) = k_data;
    r(5) = 130; // anywhere inside the second line
    run({"dcbz r4, r5", "dcbt r4, r5", "dcbf r4, r5", "dcbst r4, r5", "dcbtst r4, r5",
         "icbi r4, r5"});
    CHECK(memory.read<u64>(k_data + 120) == ~u64{0});
    CHECK(memory.read<u64>(k_data + 128) == 0);
    CHECK(memory.read<u64>(k_data + 248) == 0);
}

// ---- Faults ----

TEST_CASE_METHOD(PpuTest, "a bad access faults before changing any register") {
    r(4) = 0x9000'0000; // unmapped
    run({"lwzu r3, 8(r4)"});
    CHECK(stopped_with(StopReason::fault));
    CHECK(state().pc == k_code);
    CHECK(r(4) == 0x9000'0000);
}

TEST_CASE_METHOD(PpuTest, "stores to read-only memory fault") {
    memory.protect(k_data, Memory::k_page_size, Memory::k_read);
    r(4) = k_data;
    run({"stw r3, 0(r4)"});
    CHECK(stopped_with(StopReason::fault));
}

TEST_CASE_METHOD(PpuTest, "addresses above 4 GB fault") {
    r(4) = 0x1'0000'0000 + k_data;
    run({"lbz r3, 0(r4)"});
    CHECK(stopped_with(StopReason::fault));
}
