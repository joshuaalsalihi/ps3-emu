#include "support/spu_test.h"

#include <catch2/catch_test_macros.hpp>

using spu::StopReason;
using test::SpuTest;

// ---- Compares ----

namespace {

const Vector k_a = Vector::from_words(0x01ff'8000, 0x7fff'ffff, 5, 0);
const Vector k_b = Vector::from_words(0x01fe'8000, 0x8000'0000, 5, 1);

} // namespace

TEST_CASE_METHOD(SpuTest, "word compares: equal, signed and logical greater than") {
    r(1) = k_a;
    r(2) = k_b;
    run({"ceq $3, $1, $2", "cgt $4, $1, $2", "clgt $5, $1, $2", "ceqi $6, $1, 5", "cgti $7, $1, -1",
         "clgti $8, $1, -1"});
    CHECK(r(3) == Vector::from_words(0, 0, ~0u, 0));
    CHECK(r(4) == Vector::from_words(~0u, ~0u, 0, 0));
    CHECK(r(5) == Vector::from_words(~0u, 0, 0, 0));
    CHECK(r(6) == Vector::from_words(0, 0, ~0u, 0));
    CHECK(r(7) == Vector::from_words(~0u, ~0u, ~0u, ~0u));
    CHECK(r(8) == Vector{}); // nothing is logically greater than 0xffffffff
}

TEST_CASE_METHOD(SpuTest, "halfword compares") {
    r(1) = k_a;
    r(2) = k_b;
    run({"ceqh $3, $1, $2", "cgth $4, $1, $2", "clgth $5, $1, $2", "ceqhi $6, $1, 0",
         "cgthi $7, $1, -1", "clgthi $8, $1, 511"});
    CHECK(r(3).word(0) == 0x0000'ffff);
    CHECK(r(4).word(0) == 0xffff'0000);
    CHECK(r(4).word(1) == 0xffff'0000);
    CHECK(r(5).word(1) == 0x0000'ffff);
    CHECK(r(6).word(0) == 0);
    CHECK(r(6).word(3) == 0xffff'ffff);
    CHECK(r(7).word(0) == 0xffff'0000);
    CHECK(r(8).word(0) == 0x0000'ffff);
}

TEST_CASE_METHOD(SpuTest, "byte compares use the low byte of the immediate") {
    r(1) = k_a;
    r(2) = k_b;
    run({"ceqb $3, $1, $2", "cgtb $4, $1, $2", "clgtb $5, $1, $2", "ceqbi $6, $1, -1",
         "cgtbi $7, $1, 0", "clgtbi $8, $1, 127"});
    CHECK(r(3).word(0) == 0xff00'ffff);
    CHECK(r(4).word(0) == 0x00ff'0000);
    CHECK(r(4).word(1) == 0xff00'0000);
    CHECK(r(5).word(1) == 0x00ff'ffff);
    CHECK(r(6).word(0) == 0x00ff'0000);
    CHECK(r(7).word(0) == 0xff00'0000);
    CHECK(r(8).word(0) == 0x00ff'ff00);
}

// ---- Branches ----

TEST_CASE_METHOD(SpuTest, "relative and absolute branches") {
    run({"il $3, 1", "br 0xc", "il $3, 2", "ai $3, $3, 10"});
    CHECK(preferred(3) == 11);
    run({"il $3, 1", "bra 0xc", "il $3, 2", "ai $3, $3, 10"});
    CHECK(preferred(3) == 11);
}

TEST_CASE_METHOD(SpuTest, "branch and set link stores the return address in the preferred slot") {
    set_all(0, 0xffff'ffff);
    run({"il $3, 1", "brsl $0, 0xc", "il $3, 2", "ai $3, $3, 10"});
    CHECK(r(0) == Vector::from_words(8, 0, 0, 0));
    CHECK(preferred(3) == 11);
    run({"brasl $1, 0x8", "il $3, 2", "nop $0"});
    CHECK(r(1) == Vector::from_words(4, 0, 0, 0));
}

TEST_CASE_METHOD(SpuTest, "a counted loop with brnz") {
    run({"il $3, 5", "il $4, 0", "ai $4, $4, 2", "ai $3, $3, -1", "brnz $3, 0x8"});
    CHECK(preferred(3) == 0);
    CHECK(preferred(4) == 10);
}

TEST_CASE_METHOD(SpuTest, "conditional branches test the preferred word or halfword") {
    set_all(3, 0x0001'0000); // nonzero word, zero halfword
    run({"brz $3, 0x8", "il $4, 1", "brhz $3, 0x10", "il $5, 1", "nop $0"});
    CHECK(preferred(4) == 1); // brz not taken
    CHECK(preferred(5) == 0); // brhz taken
    run({"brnz $3, 0x8", "il $6, 1", "brhnz $3, 0x10", "il $7, 1", "nop $0"});
    CHECK(preferred(6) == 0); // brnz taken
    CHECK(preferred(7) == 1); // brhnz not taken
}

TEST_CASE_METHOD(SpuTest, "indirect branches ignore the low two address bits") {
    set_all(5, 0xc | 3);
    run({"bi $5", "il $3, 2", "il $3, 3", "il $4, 7"});
    CHECK(preferred(3) == 0);
    CHECK(preferred(4) == 7);
}

TEST_CASE_METHOD(SpuTest, "bisl reads its target before writing the link") {
    set_all(5, 0x8);
    run({"bisl $5, $5", "il $3, 1", "nop $0"});
    CHECK(preferred(3) == 0);
    CHECK(r(5) == Vector::from_words(4, 0, 0, 0));
}

TEST_CASE_METHOD(SpuTest, "indirect branches can enable or disable interrupts") {
    set_all(5, 0x4);
    run({"bie $5"});
    CHECK(thread.state.interrupts_enabled);
    run({"bid $5"});
    CHECK_FALSE(thread.state.interrupts_enabled);
}

TEST_CASE_METHOD(SpuTest, "bisled branches only when an event is pending") {
    set_all(5, 0x8);
    run({"bisled $0, $5", "il $3, 1", "nop $0"});
    CHECK(preferred(3) == 1);
    CHECK(preferred(0) == 4); // the link is written either way

    thread.event_pending = true;
    set_all(3, 0);
    run({"bisled $0, $5", "il $3, 1", "nop $0"});
    CHECK(preferred(3) == 0);
}

TEST_CASE_METHOD(SpuTest, "iret returns to SRR0") {
    thread.state.srr0 = 0x8;
    run({"iret $0", "il $3, 1", "nop $0"});
    CHECK(preferred(3) == 0);
}

TEST_CASE_METHOD(SpuTest, "indirect conditional branches") {
    set_all(3, 0x0001'0000); // nonzero word, zero halfword
    set_all(5, 0x8);
    set_all(6, 0x10);
    run({"biz $3, $5", "il $4, 1", "bihz $3, $6", "il $7, 1", "nop $0"});
    CHECK(preferred(4) == 1); // biz not taken
    CHECK(preferred(7) == 0); // bihz taken

    run({"binz $3, $5", "il $8, 1", "bihnz $3, $6", "il $9, 1", "nop $0"});
    CHECK(preferred(8) == 0); // binz taken
    CHECK(preferred(9) == 1); // bihnz not taken
}

// ---- Halts ----

TEST_CASE_METHOD(SpuTest, "a halt stops the SPU on the instruction when its condition holds") {
    set_all(1, 5);
    set_all(2, 5);
    run({"nop $0", "heq $1, $2"});
    CHECK(stopped_with(StopReason::halt));
    CHECK(thread.state.pc == 4);
}

TEST_CASE_METHOD(SpuTest, "halts compare signed or logically") {
    set_all(1, 0xffff'ffff);
    set_all(2, 1);
    run({"hgt $1, $2", "hgti $1, 0", "heqi $1, 0"});
    CHECK_FALSE(thread.stop); // -1 is not greater than 1 or 0
    run({"hlgt $1, $2"});
    CHECK(stopped_with(StopReason::halt));
    run({"hlgti $2, -1"}); // 1 is not logically greater than 0xffffffff
    CHECK_FALSE(thread.stop);
    run({"heqi $1, -1"});
    CHECK(stopped_with(StopReason::halt));
}

// ---- Hints ----

TEST_CASE_METHOD(SpuTest, "branch hints change nothing") {
    set_all(5, 0x10);
    run({"hbr 0x8, $5", "hbrp", "hbra 0x10, 0x100", "hbrr 0x10, 0x100", "nop $0"});
    CHECK_FALSE(thread.stop);
    CHECK(thread.state.pc == 0x14);
    CHECK(r(5) == Vector::from_words(0x10, 0x10, 0x10, 0x10));
}
