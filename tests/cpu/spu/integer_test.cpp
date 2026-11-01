#include "support/spu_test.h"

#include <catch2/catch_test_macros.hpp>

using test::SpuTest;

TEST_CASE_METHOD(SpuTest, "word add and subtract wrap around") {
    set_words(1, 1, 0xffff'ffff, 0x8000'0000, 5);
    set_words(2, 2, 1, 0x8000'0000, 7);
    run({"a $3, $1, $2", "sf $4, $1, $2", "ai $5, $1, -1", "sfi $6, $1, 10"});
    CHECK(r(3) == Vector::from_words(3, 0, 0, 12));
    CHECK(r(4) == Vector::from_words(1, 2, 0, 2)); // "subtract from": RB - RA
    CHECK(r(5) == Vector::from_words(0, 0xffff'fffe, 0x7fff'ffff, 4));
    CHECK(r(6) == Vector::from_words(9, 11, 0x8000'000a, 5));
}

TEST_CASE_METHOD(SpuTest, "halfword add and subtract work on each halfword") {
    set_words(7, 0x0001'ffff, 0x8000'7fff, 0, 0);
    set_words(8, 0x0001'0001, 0x8000'0001, 0, 0);
    run({"ah $9, $7, $8", "sfh $10, $7, $8", "ahi $11, $7, -1", "sfhi $12, $7, 1"});
    CHECK(r(9) == Vector::from_words(0x0002'0000, 0x0000'8000, 0, 0));
    CHECK(r(10) == Vector::from_words(0x0000'0002, 0x0000'8002, 0, 0));
    CHECK(r(11) == Vector::from_words(0x0000'fffe, 0x7fff'7ffe, 0xffff'ffff, 0xffff'ffff));
    CHECK(r(12) == Vector::from_words(0x0000'0002, 0x8001'8002, 0x0001'0001, 0x0001'0001));
}

TEST_CASE_METHOD(SpuTest, "cg and addx chain a 64-bit addition") {
    // 0x00000001_ffffffff + 1 in the first doubleword.
    set_words(1, 1, 0xffff'ffff, 0, 0);
    set_words(2, 0, 1, 0, 0);
    run({"cg $4, $1, $2", "shlqbyi $5, $4, 4", "addx $5, $1, $2"});
    CHECK(r(4) == Vector::from_words(0, 1, 0, 0));
    CHECK(r(5) == Vector::from_words(2, 0, 0, 0));
}

TEST_CASE_METHOD(SpuTest, "extended carry and borrow use the low bit of RT") {
    set_words(1, 0xffff'ffff, 0xffff'ffff, 0x8000'0000, 0);
    set_words(2, 0, 0, 0x8000'0000, 0);
    set_words(3, 1, 0, 0, 0xffff'fffe); // only bit 0 of each word counts
    run({"cgx $3, $1, $2"});
    CHECK(r(3) == Vector::from_words(1, 0, 1, 0));

    set_words(1, 5, 5, 0, 0);
    set_words(2, 7, 7, 0, 0);
    set_words(4, 1, 0, 1, 0); // 1 means "no borrow"
    run({"sfx $4, $1, $2"});
    CHECK(r(4) == Vector::from_words(2, 1, 0, 0xffff'ffff));
}

TEST_CASE_METHOD(SpuTest, "borrow generate reports whether RB - RA needs no borrow") {
    set_words(1, 5, 7, 1, 0);
    set_words(2, 7, 5, 1, 0);
    run({"bg $3, $1, $2"});
    CHECK(r(3) == Vector::from_words(1, 0, 1, 1));

    set_all(1, 5);
    set_words(2, 5, 4, 5, 6);
    set_words(4, 1, 1, 0, 0);
    run({"bgx $4, $1, $2"});
    CHECK(r(4) == Vector::from_words(1, 0, 0, 1));
}

TEST_CASE_METHOD(SpuTest, "16-bit multiplies use the low halfwords") {
    set_words(1, 0x0000'fffe, 0x0000'7fff, 0x1234'8000, 3);
    set_words(2, 0x0000'0003, 0x0000'7fff, 0x5678'8000, 0xffff'0002);
    set_words(3, 10, 0, 0, 0);
    run({"mpy $4, $1, $2", "mpyu $5, $1, $2", "mpyi $6, $1, -3", "mpyui $7, $1, -3",
         "mpya $8, $1, $2, $3", "mpys $9, $1, $2"});
    CHECK(r(4) == Vector::from_words(0xffff'fffa, 0x3fff'0001, 0x4000'0000, 6));
    CHECK(r(5) == Vector::from_words(0x0002'fffa, 0x3fff'0001, 0x4000'0000, 6));
    CHECK(r(6) == Vector::from_words(6, 0xfffe'8003, 0x0001'8000, 0xffff'fff7));
    // mpyui sign-extends the immediate to 16 bits (0xfffd), then multiplies unsigned.
    CHECK(r(7) == Vector::from_words(0xfffb'0006, 0x7ffd'8003, 0x7ffe'8000, 0x0002'fff7));
    CHECK(r(8) == Vector::from_words(4, 0x3fff'0001, 0x4000'0000, 6));
    CHECK(r(9) == Vector::from_words(0xffff'ffff, 0x3fff, 0x4000, 0));
}

TEST_CASE_METHOD(SpuTest, "mpyh multiplies the high halfword of RA by the low of RB") {
    set_words(10, 0x0003'0000, 0x1234'0000, 0xffff'0000, 0);
    set_words(11, 5, 3, 2, 0);
    run({"mpyh $12, $10, $11"});
    CHECK(r(12) == Vector::from_words(0x000f'0000, 0x369c'0000, 0xfffe'0000, 0));
}

TEST_CASE_METHOD(SpuTest, "high-halfword multiplies, with and without accumulation") {
    set_words(1, 0xfffe'0000, 0x7fff'1234, 0x8000'0000, 0x0003'0000);
    set_words(2, 0x0003'0000, 0x7fff'5678, 0x8000'0000, 0xffff'0000);
    set_all(5, 1);
    set_all(6, 1);
    run({"mpyhh $3, $1, $2", "mpyhhu $4, $1, $2", "mpyhha $5, $1, $2", "mpyhhau $6, $1, $2"});
    CHECK(r(3) == Vector::from_words(0xffff'fffa, 0x3fff'0001, 0x4000'0000, 0xffff'fffd));
    CHECK(r(4) == Vector::from_words(0x0002'fffa, 0x3fff'0001, 0x4000'0000, 0x0002'fffd));
    CHECK(r(5) == Vector::from_words(0xffff'fffb, 0x3fff'0002, 0x4000'0001, 0xffff'fffe));
    CHECK(r(6) == Vector::from_words(0x0002'fffb, 0x3fff'0002, 0x4000'0001, 0x0002'fffe));
}

TEST_CASE_METHOD(SpuTest, "counting bits") {
    set_words(1, 0, 1, 0x8000'0000, 0x0001'0000);
    set_words(2, 0xff00'f00f, 0x0102'0304, 0, 0xffff'ffff);
    run({"clz $3, $1", "cntb $4, $2"});
    CHECK(r(3) == Vector::from_words(32, 31, 0, 15));
    CHECK(r(4) == Vector::from_words(0x0800'0404, 0x0101'0201, 0, 0x0808'0808));
}

TEST_CASE_METHOD(SpuTest, "form select masks expand bits of the preferred slot") {
    set_all(1, 0x1234'a5c3); // fsmb uses the low 16 bits
    set_all(2, 0x1234'56a5); // fsmh uses the low 8 bits
    set_all(3, 0xffff'fff5); // fsm uses the low 4 bits
    run({"fsmb $4, $1", "fsmh $5, $2", "fsm $6, $3"});
    CHECK(r(4) == Vector::from_words(0xff00'ff00, 0x00ff'00ff, 0xffff'0000, 0x0000'ffff));
    CHECK(r(5) == Vector::from_words(0xffff'0000, 0xffff'0000, 0x0000'ffff, 0x0000'ffff));
    CHECK(r(6) == Vector::from_words(0, 0xffff'ffff, 0, 0xffff'ffff));
}

TEST_CASE_METHOD(SpuTest, "gather bits collects the low bit of each element") {
    set_words(1, 0x0100'0302, 0x0001'0001, 0xffff'ffff, 0xfe00'0000);
    run({"gbb $2, $1", "gbh $3, $1", "gb $4, $1"});
    CHECK(r(2) == Vector::from_words(0xa5f0, 0, 0, 0));
    CHECK(r(3) == Vector::from_words(0x3c, 0, 0, 0));
    CHECK(r(4) == Vector::from_words(0x6, 0, 0, 0));
}

TEST_CASE_METHOD(SpuTest, "byte averages, differences and sums") {
    set_words(1, 0xff00'0180, 0x0102'0304, 0, 0);
    set_words(2, 0xff01'027f, 0xffff'ffff, 0, 0);
    run({"avgb $3, $1, $2", "absdb $4, $1, $2", "sumb $5, $1, $2"});
    CHECK(r(3).word(0) == 0xff01'0280); // rounds up
    CHECK(r(4).word(0) == 0x0001'0101);
    // Each word: the sum of RB's bytes in the high halfword, RA's in the low.
    CHECK(r(5) == Vector::from_words(0x0181'0180, 0x03fc'000a, 0, 0));
}

TEST_CASE_METHOD(SpuTest, "sign extension") {
    set_words(1, 0x1234'8081, 0, 0x8765'4321, 0x0000'0001);
    set_words(2, 0x1234'5678, 0x8765'4321, 0xffff'ffff, 0x0000'0001);
    run({"xsbh $3, $1", "xshw $4, $1", "xswd $5, $2"});
    CHECK(r(3).word(0) == 0x0034'ff81);
    CHECK(r(4).word(0) == 0xffff'8081);
    CHECK(r(5) == Vector::from_dwords(0xffff'ffff'8765'4321, 0x0000'0000'0000'0001));
}

TEST_CASE_METHOD(SpuTest, "bitwise logic") {
    set_all(1, 0xf0f0'f0f0);
    set_all(2, 0xff00'ff00);
    run({"and $3, $1, $2", "andc $4, $1, $2", "or $5, $1, $2", "orc $6, $1, $2", "xor $7, $1, $2",
         "nand $8, $1, $2", "nor $9, $1, $2", "eqv $10, $1, $2"});
    CHECK(r(3).word(3) == 0xf000'f000);
    CHECK(r(4).word(3) == 0x00f0'00f0);
    CHECK(r(5).word(3) == 0xfff0'fff0);
    CHECK(r(6).word(3) == 0xf0ff'f0ff);
    CHECK(r(7).word(3) == 0x0ff0'0ff0);
    CHECK(r(8).word(3) == 0x0fff'0fff);
    CHECK(r(9).word(3) == 0x000f'000f);
    CHECK(r(10).word(3) == 0xf00f'f00f);
}

TEST_CASE_METHOD(SpuTest, "logical immediates are sign-extended to the element size") {
    set_all(1, 0x1234'5678);
    run({"andbi $2, $1, 15", "andbi $3, $1, -16", "andhi $4, $1, -16", "andi $5, $1, 511",
         "orbi $6, $1, 128", "orhi $7, $1, 1", "ori $8, $1, -1", "xorbi $9, $1, 255",
         "xorhi $10, $1, -1", "xori $11, $1, 1"});
    CHECK(r(2).word(1) == 0x0204'0608);
    CHECK(r(3).word(1) == 0x1030'5070); // byte forms use the low 8 bits of I10
    CHECK(r(4).word(1) == 0x1230'5670);
    CHECK(r(5).word(1) == 0x0000'0078);
    CHECK(r(6).word(1) == 0x92b4'd6f8);
    CHECK(r(7).word(1) == 0x1235'5679);
    CHECK(r(8).word(1) == 0xffff'ffff);
    CHECK(r(9).word(1) == 0xedcb'a987);
    CHECK(r(10).word(1) == 0xedcb'a987);
    CHECK(r(11).word(1) == 0x1234'5679);
}

TEST_CASE_METHOD(SpuTest, "orx ORs the four words into the preferred slot") {
    set_words(1, 1, 2, 4, 0x8000'0000);
    run({"orx $2, $1"});
    CHECK(r(2) == Vector::from_words(0x8000'0007, 0, 0, 0));
}

TEST_CASE_METHOD(SpuTest, "selb takes bits from RB where RC is set") {
    set_all(1, 0x1234'5678);
    set_all(2, 0x9abc'def0);
    set_all(3, 0xffff'0000);
    run({"selb $4, $1, $2, $3"});
    CHECK(r(4).word(2) == 0x9abc'5678);
}

TEST_CASE_METHOD(SpuTest, "shufb selects bytes and constants") {
    set_words(1, 0x0001'0203, 0x0405'0607, 0x0809'0a0b, 0x0c0d'0e0f);
    set_words(2, 0x1011'1213, 0x1415'1617, 0x1819'1a1b, 0x1c1d'1e1f);
    // 0x80 -> 0x00, 0xc0 -> 0xff, 0xe0 -> 0x80; otherwise the low 5 bits index RA:RB.
    set_words(3, 0x001f'80c0, 0xe010'2f7f, 0xbfdf'ff05, 0x0a1b'0001);
    run({"shufb $4, $1, $2, $3"});
    CHECK(r(4) == Vector::from_words(0x001f'00ff, 0x8010'0f1f, 0x00ff'8005, 0x0a1b'0001));
}
