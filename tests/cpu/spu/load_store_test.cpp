#include "support/spu_test.h"

#include <catch2/catch_test_macros.hpp>

using test::SpuTest;

namespace {

const Vector k_pattern = Vector::from_words(0x0011'2233, 0x4455'6677, 0x8899'aabb, 0xccdd'eeff);

} // namespace

TEST_CASE_METHOD(SpuTest, "lqd and stqd ignore the low four address bits") {
    set_all(1, k_data + 5);
    r(2) = k_pattern;
    run({"stqd $2, 16($1)", "lqd $3, 16($1)"});
    CHECK(thread.local_store.read<u32>(k_data + 0x10) == 0x0011'2233);
    CHECK(thread.local_store.read<u32>(k_data + 0x1c) == 0xccdd'eeff);
    CHECK(r(3) == k_pattern);
}

TEST_CASE_METHOD(SpuTest, "the d-form offset counts quadwords and can be negative") {
    set_all(1, k_data + 0x40);
    r(2) = k_pattern;
    run({"stqd $2, -32($1)"});
    CHECK(thread.local_store.read_quad(k_data + 0x20) == k_pattern);
}

TEST_CASE_METHOD(SpuTest, "lqx and stqx add two registers, wrapping around local store") {
    set_all(1, 0x3fff0);
    set_all(4, 0x1010);
    r(2) = k_pattern;
    run({"stqx $2, $1, $4", "lqx $3, $4, $1"}); // 0x3fff0 + 0x1010 wraps to 0x1000
    CHECK(thread.local_store.read_quad(k_data) == k_pattern);
    CHECK(r(3) == k_pattern);
}

TEST_CASE_METHOD(SpuTest, "lqa and stqa use an absolute address") {
    r(2) = k_pattern;
    run({"stqa $2, 0x1020", "lqa $3, 0x1020"});
    CHECK(thread.local_store.read_quad(0x1020) == k_pattern);
    CHECK(r(3) == k_pattern);
}

TEST_CASE_METHOD(SpuTest, "lqr and stqr are relative to the instruction") {
    r(2) = k_pattern;
    run({"nop $0", "stqr $2, 0x1000", "lqr $3, 0x1000"});
    CHECK(thread.local_store.read_quad(0x1000) == k_pattern);
    CHECK(r(3) == k_pattern);
}

TEST_CASE_METHOD(SpuTest, "generate controls for insertion") {
    set_all(4, k_data + 3);
    set_all(5, k_data + 7);
    set_all(6, k_data + 0xd);
    set_all(7, k_data + 9);
    run({"cbd $10, 2($4)", "chd $11, 0($5)", "cwd $12, 0($6)", "cdd $13, 0($7)"});
    // The addressed element selects the preferred slot (bytes 0x00-0x03); every other byte
    // selects the matching byte of the second shufb operand (0x10-0x1f).
    CHECK(r(10) == Vector::from_words(0x1011'1213, 0x1403'1617, 0x1819'1a1b, 0x1c1d'1e1f));
    CHECK(r(11) == Vector::from_words(0x1011'1213, 0x1415'0203, 0x1819'1a1b, 0x1c1d'1e1f));
    CHECK(r(12) == Vector::from_words(0x1011'1213, 0x1415'1617, 0x1819'1a1b, 0x0001'0203));
    CHECK(r(13) == Vector::from_words(0x1011'1213, 0x1415'1617, 0x0001'0203, 0x0405'0607));
}

TEST_CASE_METHOD(SpuTest, "the x-form controls add two registers") {
    set_all(4, 0x10);
    set_all(5, 0x2);
    run({"cbx $10, $4, $5", "chx $11, $4, $5", "cwx $12, $4, $5", "cdx $13, $4, $5"});
    CHECK(r(10).byte(2) == 0x03);
    CHECK(r(11).half(1) == 0x0203);
    CHECK(r(12).word(0) == 0x0001'0203); // aligned down to the word
    CHECK(r(13).dword(0) == 0x0001'0203'0405'0607);
}

TEST_CASE_METHOD(SpuTest, "cbd and shufb insert a byte into a quadword") {
    set_all(4, k_data + 3);
    set_all(7, 0x0000'00ab); // the byte to insert, in the preferred slot
    r(8) = k_pattern;
    run({"cbd $5, 0($4)", "shufb $6, $7, $8, $5"});
    CHECK(r(6) == Vector::from_words(0x0011'22ab, 0x4455'6677, 0x8899'aabb, 0xccdd'eeff));
}

TEST_CASE_METHOD(SpuTest, "immediate loads fill every slot") {
    run({"il $1, -2", "ilh $2, 0x1234", "ilhu $3, 0x8000", "ila $4, 0x3ffff", "ilhu $5, 0xdead",
         "iohl $5, 0xbeef", "fsmbi $6, 0xf0f0"});
    CHECK(r(1) == Vector::from_words(0xffff'fffe, 0xffff'fffe, 0xffff'fffe, 0xffff'fffe));
    CHECK(r(2) == Vector::from_words(0x1234'1234, 0x1234'1234, 0x1234'1234, 0x1234'1234));
    CHECK(r(3) == Vector::from_words(0x8000'0000, 0x8000'0000, 0x8000'0000, 0x8000'0000));
    CHECK(r(4) == Vector::from_words(0x0003'ffff, 0x0003'ffff, 0x0003'ffff, 0x0003'ffff));
    CHECK(r(5) == Vector::from_words(0xdead'beef, 0xdead'beef, 0xdead'beef, 0xdead'beef));
    CHECK(r(6) == Vector::from_words(0xffff'ffff, 0, 0xffff'ffff, 0));
}
