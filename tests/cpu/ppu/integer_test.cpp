// Fixed-point arithmetic, logical, compare, rotate and shift instructions.

#include "support/ppu_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace ppu;
using test::PpuTest;

// ---- Add and subtract ----

TEST_CASE_METHOD(PpuTest, "addi and addis read r0 as zero") {
    r(0) = 100;
    r(1) = 0x1000;
    run({"addi r3, r0, 5", "addi r4, r1, -8", "addis r5, r0, -1", "addis r6, r1, 1"});
    CHECK(r(3) == 5);
    CHECK(r(4) == 0xff8);
    CHECK(r(5) == 0xffffffffffff0000);
    CHECK(r(6) == 0x11000);
}

TEST_CASE_METHOD(PpuTest, "add only touches XER and CR0 in its o and . forms") {
    r(4) = 0x7fffffffffffffff;
    r(5) = 1;

    SECTION("plain") {
        run({"add r3, r4, r5"});
        CHECK(r(3) == 0x8000000000000000);
        CHECK_FALSE(xer().ov);
        CHECK(state().cr == 0);
    }
    SECTION("record and overflow") {
        run({"addo. r3, r4, r5"});
        CHECK(xer().ov);
        CHECK(xer().so);
        CHECK(cr_field(0) == (k_cr_lt | k_cr_so));
    }
    SECTION("overflow clears OV but SO is sticky") {
        xer().ov = true;
        xer().so = true;
        r(5) = 0;
        run({"addo r3, r4, r5"});
        CHECK_FALSE(xer().ov);
        CHECK(xer().so);
    }
}

TEST_CASE_METHOD(PpuTest, "addc, adde, addme and addze carry out of bit 0") {
    SECTION("addc") {
        r(4) = ~u64{0};
        r(5) = 1;
        run({"addc r3, r4, r5"});
        CHECK(r(3) == 0);
        CHECK(xer().ca);
    }
    SECTION("adde adds the carry in") {
        r(4) = 1;
        r(5) = 2;
        xer().ca = true;
        run({"adde r3, r4, r5"});
        CHECK(r(3) == 4);
        CHECK_FALSE(xer().ca);
    }
    SECTION("addme adds -1 and the carry") {
        r(4) = 0;
        xer().ca = true;
        run({"addme r3, r4"});
        CHECK(r(3) == 0);
        CHECK(xer().ca);
    }
    SECTION("addze adds the carry") {
        r(4) = 5;
        xer().ca = true;
        run({"addze r3, r4"});
        CHECK(r(3) == 6);
        CHECK_FALSE(xer().ca);
    }
}

TEST_CASE_METHOD(PpuTest, "subtract-from instructions compute rB - rA") {
    SECTION("subf") {
        r(4) = 3;
        r(5) = 10;
        run({"subf r3, r4, r5"});
        CHECK(r(3) == 7);
    }
    SECTION("subfc sets CA when there is no borrow") {
        r(4) = 3;
        r(5) = 10;
        run({"subfc r3, r4, r5", "subfc r6, r5, r4"});
        CHECK(r(3) == 7);
        CHECK(r(6) == static_cast<u64>(-7));
        CHECK_FALSE(xer().ca); // from the second subtraction, which borrowed
    }
    SECTION("subfe subtracts an extra 1 when CA is clear") {
        r(4) = 3;
        r(5) = 10;
        xer().ca = false;
        run({"subfe r3, r4, r5"});
        CHECK(r(3) == 6);
    }
    SECTION("subfme and subfze") {
        r(4) = 5;
        xer().ca = true;
        run({"subfme r3, r4"});
        CHECK(r(3) == static_cast<u64>(-6));
        CHECK(xer().ca);

        xer().ca = true;
        run({"subfze r3, r4"});
        CHECK(r(3) == static_cast<u64>(-5));
        CHECK_FALSE(xer().ca);
    }
    SECTION("subfic") {
        r(4) = 3;
        run({"subfic r3, r4, 10"});
        CHECK(r(3) == 7);
        CHECK(xer().ca);
    }
}

TEST_CASE_METHOD(PpuTest, "neg overflows only for the most negative number") {
    r(4) = 5;
    r(5) = 0x8000000000000000;
    run({"nego r3, r4", "neg r6, r5"});
    CHECK(r(3) == static_cast<u64>(-5));
    CHECK(r(6) == 0x8000000000000000);

    run({"nego r6, r5"});
    CHECK(xer().ov);
}

TEST_CASE_METHOD(PpuTest, "addic and addic. set CA, and addic. records") {
    r(4) = ~u64{0};
    run({"addic. r3, r4, 1"});
    CHECK(r(3) == 0);
    CHECK(xer().ca);
    CHECK(cr_field(0) == k_cr_eq);
}

// ---- Multiply and divide ----

TEST_CASE_METHOD(PpuTest, "mullw multiplies the signed low words into 64 bits") {
    r(4) = 0x1234567800000003; // high word is ignored
    r(5) = ~u64{0};            // low word is -1
    r(6) = 0x7fffffff;
    r(7) = 2;
    run({"mullw r3, r4, r5", "mullwo r8, r6, r7"});
    CHECK(r(3) == static_cast<u64>(-3));
    CHECK(r(8) == 0xfffffffe);
    CHECK(xer().ov); // 0xfffffffe doesn't fit in a signed word
}

TEST_CASE_METHOD(PpuTest, "mulhw and mulhwu return the high word of the product") {
    r(4) = 0xffffffff;
    r(5) = 0xffffffff;
    r(6) = 0x7fffffff;
    run({"mulhwu r3, r4, r5", "mulhw r7, r6, r6"});
    CHECK(r(3) == 0xfffffffe);
    CHECK(r(7) == 0x3fffffff);
}

TEST_CASE_METHOD(PpuTest, "64-bit multiplies") {
    // Expected values cross-checked with Python's arbitrary-precision integers.
    r(4) = 0x0123456789abcdef;
    r(5) = 0xfedcba9876543210;
    run({"mulld r3, r4, r5", "mulhdu r6, r4, r5", "mulhd r7, r4, r5", "mulldo r8, r4, r5"});
    CHECK(r(3) == 0x2236d88fe5618cf0);
    CHECK(r(6) == 0x0121fa00ad77d742);
    CHECK(r(7) == 0xfffeb49923cc0953);
    CHECK(xer().ov);

    r(4) = 3;
    r(5) = static_cast<u64>(-4);
    run({"mulldo r3, r4, r5"});
    CHECK(r(3) == static_cast<u64>(-12));
    CHECK_FALSE(xer().ov);
}

TEST_CASE_METHOD(PpuTest, "mulli") {
    r(4) = 7;
    run({"mulli r3, r4, -3"});
    CHECK(r(3) == static_cast<u64>(-21));
}

TEST_CASE_METHOD(PpuTest, "divisions round toward zero") {
    r(4) = static_cast<u64>(-100);
    r(5) = 7;
    r(6) = static_cast<u64>(-7);
    r(7) = 2;
    r(8) = 0xfffffff0;
    r(9) = 3;
    run({"divw r3, r4, r5", "divd r10, r6, r7", "divwu r11, r8, r9", "divdu r12, r5, r7"});
    CHECK(static_cast<u32>(r(3)) == static_cast<u32>(-14));
    CHECK(r(10) == static_cast<u64>(-3));
    CHECK(r(11) == 0x55555550);
    CHECK(r(12) == 3);
}

TEST_CASE_METHOD(PpuTest, "division overflow sets OV") {
    // UNVERIFIED: rD is undefined in these cases; we return 0.
    r(4) = 10;
    r(5) = 0;
    r(6) = 0x80000000;
    r(7) = ~u64{0};

    SECTION("divide by zero") {
        run({"divwo r3, r4, r5"});
        CHECK(xer().ov);
        CHECK(r(3) == 0);
    }
    SECTION("most negative word divided by -1") {
        run({"divwo r3, r6, r7"});
        CHECK(xer().ov);
    }
    SECTION("unsigned doubleword divide by zero") {
        run({"divduo. r3, r4, r5"});
        CHECK(xer().ov);
        CHECK(cr_field(0) == (k_cr_eq | k_cr_so));
    }
}

// ---- Compare ----

TEST_CASE_METHOD(PpuTest, "compares set one CR field") {
    SECTION("signed vs unsigned") {
        r(3) = ~u64{0};
        r(4) = 1;
        run({"cmpw r3, r4", "cmplw cr1, r3, r4"});
        CHECK(cr_field(0) == k_cr_lt);
        CHECK(cr_field(1) == k_cr_gt);
    }
    SECTION("word compares ignore the high word") {
        r(3) = 0x0000000100000000;
        r(4) = 0;
        run({"cmpw r3, r4", "cmpd cr7, r3, r4"});
        CHECK(cr_field(0) == k_cr_eq);
        CHECK(cr_field(7) == k_cr_gt);
    }
    SECTION("immediates") {
        r(3) = ~u64{0};
        run({"cmpwi cr2, r3, -1", "cmpldi cr3, r3, 0x10"});
        CHECK(cr_field(2) == k_cr_eq);
        CHECK(cr_field(3) == k_cr_gt);
        CHECK(cr_field(0) == 0);
    }
    SECTION("SO is copied from XER") {
        xer().so = true;
        run({"cmpdi r3, 0"});
        CHECK(cr_field(0) == (k_cr_eq | k_cr_so));
    }
}

// ---- Logical ----

TEST_CASE_METHOD(PpuTest, "logical operations") {
    r(4) = 0xff00ff00ff00ff00;
    r(5) = 0x0ff00ff00ff00ff0;
    run({"and r10, r4, r5", "andc r11, r4, r5", "or r12, r4, r5", "orc r13, r4, r5",
         "xor r14, r4, r5", "nand r15, r4, r5", "nor r16, r4, r5", "eqv r17, r4, r5"});
    CHECK(r(10) == 0x0f000f000f000f00);
    CHECK(r(11) == 0xf000f000f000f000);
    CHECK(r(12) == 0xfff0fff0fff0fff0);
    CHECK(r(13) == 0xff0fff0fff0fff0f);
    CHECK(r(14) == 0xf0f0f0f0f0f0f0f0);
    CHECK(r(15) == 0xf0fff0fff0fff0ff);
    CHECK(r(16) == 0x000f000f000f000f);
    CHECK(r(17) == 0x0f0f0f0f0f0f0f0f);
}

TEST_CASE_METHOD(PpuTest, "logical immediates are zero-extended") {
    r(4) = 0xff00ff00ff00ff00;
    run({"andi. r3, r4, 0xff00", "andis. r5, r4, 0xff00", "oris r6, r0, 0xffff",
         "xoris r7, r4, 0xffff", "ori r8, r0, 0x1234", "xori r9, r4, 0xffff"});
    CHECK(r(3) == 0xff00);
    CHECK(r(5) == 0xff000000);
    CHECK(cr_field(0) == k_cr_gt); // from andis.
    CHECK(r(6) == 0xffff0000);
    CHECK(r(7) == 0xff00ff0000ffff00);
    CHECK(r(8) == 0x1234);
    CHECK(r(9) == 0xff00ff00ff0000ff);
}

TEST_CASE_METHOD(PpuTest, "and. records a negative result") {
    r(4) = ~u64{0};
    run({"and. r3, r4, r4"});
    CHECK(cr_field(0) == k_cr_lt);
}

TEST_CASE_METHOD(PpuTest, "sign extension") {
    r(4) = 0x80;
    r(5) = 0x7fff;
    r(6) = 0x80000000;
    run({"extsb. r3, r4", "extsh r7, r5", "extsw r8, r6"});
    CHECK(r(3) == 0xffffffffffffff80);
    CHECK(cr_field(0) == k_cr_lt);
    CHECK(r(7) == 0x7fff);
    CHECK(r(8) == 0xffffffff80000000);
}

TEST_CASE_METHOD(PpuTest, "count leading zeros") {
    r(4) = 0;
    r(5) = 0xffffffff00000001; // cntlzw only looks at the low word
    r(6) = 1;
    run({"cntlzw r10, r4", "cntlzw r11, r5", "cntlzd r12, r4", "cntlzd r13, r6"});
    CHECK(r(10) == 32);
    CHECK(r(11) == 31);
    CHECK(r(12) == 64);
    CHECK(r(13) == 63);
}

// ---- Rotate ----

TEST_CASE_METHOD(PpuTest, "rlwinm") {
    SECTION("shift left by 2") {
        r(4) = 0x40000001;
        run({"rlwinm r3, r4, 2, 0, 29"});
        CHECK(r(3) == 4);
    }
    SECTION("a wrapping mask includes the duplicated high word") {
        // MB > ME makes the mask wrap through bits 0-31, where ROTL32 has a copy of the word.
        r(4) = 0x12345678;
        run({"rlwinm r3, r4, 0, 28, 3"});
        CHECK(r(3) == 0x1234567810000008);
    }
}

TEST_CASE_METHOD(PpuTest, "rlwnm uses the low 5 bits of rB") {
    r(4) = 0x12345678;
    r(5) = 36;
    run({"rlwnm r3, r4, r5, 0, 31"});
    CHECK(r(3) == 0x23456781);
}

TEST_CASE_METHOD(PpuTest, "rlwimi inserts under the mask") {
    r(3) = 0xaaaaaaaaaaaaaaaa;
    r(4) = 0xff;
    run({"rlwimi r3, r4, 8, 16, 23"});
    CHECK(r(3) == 0xaaaaaaaaaaaaffaa);
}

TEST_CASE_METHOD(PpuTest, "64-bit rotates") {
    r(4) = 0x0123456789abcdef;
    r(5) = 68; // rldcl uses the low 6 bits: 4
    r(6) = 8;
    r(20) = ~u64{0};
    r(21) = 0x1234;
    run({"rldicl r10, r4, 8, 48", "rldicr r11, r4, 4, 59", "rldic r12, r4, 8, 40",
         "rldcl r13, r4, r5, 0", "rldcr r14, r4, r6, 7", "rldimi r20, r21, 16, 32"});
    CHECK(r(10) == 0xef01);
    CHECK(r(11) == 0x123456789abcdef0);
    CHECK(r(12) == 0xcdef00);
    CHECK(r(13) == 0x123456789abcdef0);
    CHECK(r(14) == 0x2300000000000000);
    CHECK(r(20) == 0xffffffff1234ffff);
}

// ---- Shift ----

TEST_CASE_METHOD(PpuTest, "word shifts use the low word and 6-bit amounts") {
    r(4) = 0xffffffff80000001;
    r(5) = 1;
    r(6) = 32;
    r(7) = 4;
    run({"slw r10, r4, r5", "slw r11, r4, r6", "srw r12, r4, r7", "srw r13, r4, r6"});
    CHECK(r(10) == 2);
    CHECK(r(11) == 0);
    CHECK(r(12) == 0x08000000);
    CHECK(r(13) == 0);
}

TEST_CASE_METHOD(PpuTest, "doubleword shifts use 7-bit amounts") {
    r(4) = 1;
    r(5) = 63;
    r(6) = 64;
    r(7) = 128; // only the low 7 bits count, so this shifts by 0
    r(8) = 0x8000000000000000;
    run({"sld r10, r4, r5", "sld r11, r4, r6", "sld r12, r4, r7", "srd r13, r8, r5"});
    CHECK(r(10) == 0x8000000000000000);
    CHECK(r(11) == 0);
    CHECK(r(12) == 1);
    CHECK(r(13) == 1);
}

TEST_CASE_METHOD(PpuTest, "sraw sets CA when a negative value loses one bits") {
    r(5) = 4;
    SECTION("no one bits lost") {
        r(4) = 0x80000000;
        run({"sraw r3, r4, r5"});
        CHECK(r(3) == 0xfffffffff8000000);
        CHECK_FALSE(xer().ca);
    }
    SECTION("one bits lost") {
        r(4) = 0x80000001;
        run({"sraw r3, r4, r5"});
        CHECK(xer().ca);
    }
    SECTION("shift of 32 or more fills with the sign") {
        r(4) = 0x80000000;
        r(5) = 40;
        run({"sraw r3, r4, r5"});
        CHECK(r(3) == ~u64{0});
        CHECK(xer().ca);
    }
    SECTION("positive values never set CA") {
        r(4) = 0x7fffffff;
        r(5) = 40;
        run({"sraw r3, r4, r5"});
        CHECK(r(3) == 0);
        CHECK_FALSE(xer().ca);
    }
}

TEST_CASE_METHOD(PpuTest, "srawi") {
    r(4) = ~u64{0};
    run({"srawi r3, r4, 1"});
    CHECK(r(3) == ~u64{0});
    CHECK(xer().ca);
}

TEST_CASE_METHOD(PpuTest, "srad and sradi") {
    r(4) = static_cast<u64>(-5);
    r(5) = 1;
    r(6) = 0x1234567800000000;
    r(7) = 0x8000000000000000;
    run({"srad r10, r4, r5", "sradi r11, r6, 32", "sradi r12, r7, 63"});
    CHECK(r(10) == static_cast<u64>(-3)); // rounds toward minus infinity
    CHECK(r(11) == 0x12345678);
    CHECK(r(12) == ~u64{0});
    CHECK_FALSE(xer().ca); // the last shift only lost zero bits
}
