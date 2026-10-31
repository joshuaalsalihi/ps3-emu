// Assembler and disassembler, checked against encodings from LLVM's PowerPC assembler
// (llvm-mc -triple=powerpc64 -show-encoding), an independent implementation.

#include "cpu/ppu/assembler.h"
#include "cpu/ppu/disassembler.h"
#include "cpu/ppu/opcodes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <set>
#include <string_view>

using namespace ppu;
using Catch::Matchers::ContainsSubstring;

namespace {

struct Encoding {
    std::string_view text; // as our disassembler prints it, at address 0
    u32 raw;               // as LLVM encodes it
};

// clang-format off
constexpr Encoding k_encodings[] = {
    {"addi r3, r1, -8", 0x3861fff8},
    {"addis r3, r2, -1", 0x3c62ffff},
    {"addic r3, r4, 100", 0x30640064},
    {"addic. r3, r4, -1", 0x3464ffff},
    {"subfic r3, r4, 10", 0x2064000a},
    {"mulli r3, r4, -3", 0x1c64fffd},
    {"add r3, r4, r5", 0x7c642a14},
    {"add. r3, r4, r5", 0x7c642a15},
    {"addo. r3, r4, r5", 0x7c642e15},
    {"addc r3, r4, r5", 0x7c642814},
    {"adde r3, r4, r5", 0x7c642914},
    {"addme r3, r4", 0x7c6401d4},
    {"addze r3, r4", 0x7c640194},
    {"subf r3, r4, r5", 0x7c642850},
    {"subfc r3, r4, r5", 0x7c642810},
    {"subfe r3, r4, r5", 0x7c642910},
    {"subfme r3, r4", 0x7c6401d0},
    {"subfze r3, r4", 0x7c640190},
    {"neg r3, r4", 0x7c6400d0},
    {"mullw r3, r4, r5", 0x7c6429d6},
    {"mulhw r3, r4, r5", 0x7c642896},
    {"mulhwu r3, r4, r5", 0x7c642816},
    {"mulld r3, r4, r5", 0x7c6429d2},
    {"mulhd r3, r4, r5", 0x7c642892},
    {"mulhdu r3, r4, r5", 0x7c642812},
    {"divw r3, r4, r5", 0x7c642bd6},
    {"divwu r3, r4, r5", 0x7c642b96},
    {"divd r3, r4, r5", 0x7c642bd2},
    {"divdu r3, r4, r5", 0x7c642b92},
    {"cmpw r3, r4", 0x7c032000},
    {"cmpd cr7, r3, r4", 0x7fa32000},
    {"cmplw r3, r4", 0x7c032040},
    {"cmpld cr2, r3, r4", 0x7d232040},
    {"cmpwi r3, -1", 0x2c03ffff},
    {"cmpdi cr1, r3, 5", 0x2ca30005},
    {"cmplwi r3, 0xffff", 0x2803ffff},
    {"cmpldi cr7, r3, 0x10", 0x2ba30010},
    {"andi. r3, r4, 0xff", 0x708300ff},
    {"andis. r3, r4, 0x8000", 0x74838000},
    {"ori r3, r4, 0x1234", 0x60831234},
    {"oris r3, r4, 0xffff", 0x6483ffff},
    {"xori r3, r4, 0x1", 0x68830001},
    {"xoris r3, r4, 0x10", 0x6c830010},
    {"and r3, r4, r5", 0x7c832838},
    {"and. r3, r4, r5", 0x7c832839},
    {"andc r3, r4, r5", 0x7c832878},
    {"or r3, r4, r5", 0x7c832b78},
    {"orc r3, r4, r5", 0x7c832b38},
    {"xor r3, r4, r5", 0x7c832a78},
    {"nand r3, r4, r5", 0x7c832bb8},
    {"nor r3, r4, r5", 0x7c8328f8},
    {"eqv r3, r4, r5", 0x7c832a38},
    {"extsb r3, r4", 0x7c830774},
    {"extsh. r3, r4", 0x7c830735},
    {"extsw r3, r4", 0x7c8307b4},
    {"cntlzw r3, r4", 0x7c830034},
    {"cntlzd r3, r4", 0x7c830074},
    {"rlwinm r3, r4, 2, 0, 29", 0x5483103a},
    {"rlwinm. r3, r4, 0, 31, 31", 0x548307ff},
    {"rlwnm r3, r4, r5, 0, 31", 0x5c83283e},
    {"rlwimi r3, r4, 8, 16, 23", 0x5083442e},
    {"rldicl r3, r4, 8, 48", 0x78834420},
    {"rldicr r3, r4, 3, 60", 0x78831f24},
    {"rldic r3, r4, 32, 8", 0x7883020a},
    {"rldimi r3, r4, 40, 0", 0x7883400e},
    {"rldcl r3, r4, r5, 0", 0x78832810},
    {"rldcr r3, r4, r5, 63", 0x78832ff2},
    {"slw r3, r4, r5", 0x7c832830},
    {"srw r3, r4, r5", 0x7c832c30},
    {"sld r3, r4, r5", 0x7c832836},
    {"srd r3, r4, r5", 0x7c832c36},
    {"sraw r3, r4, r5", 0x7c832e30},
    {"srawi r3, r4, 31", 0x7c83fe70},
    {"srad r3, r4, r5", 0x7c832e34},
    {"sradi r3, r4, 63", 0x7c83fe76},
    {"li r3, -1", 0x3860ffff},
    {"lis r3, 16", 0x3c600010},
    {"mr r3, r4", 0x7c832378},
    {"mr. r3, r4", 0x7c832379},
    {"nop", 0x60000000},
    {"b 0x100", 0x48000100},
    {"bl 0x100", 0x48000101},
    {"ba 0x100", 0x48000102},
    {"bla 0x2000", 0x48002003},
    {"beq 0x20", 0x41820020},
    {"bne cr7, 0x40", 0x409e0040},
    {"blt 0x8", 0x41800008},
    {"bgt cr1, 0xc", 0x4185000c},
    {"bge 0x10", 0x40800010},
    {"ble 0x14", 0x40810014},
    {"bso 0x18", 0x41830018},
    {"bns 0x1c", 0x4083001c},
    {"beql 0x20", 0x41820021},
    {"bdnz 0x10", 0x42000010},
    {"bdz 0x8", 0x42400008},
    {"bc 20, 0, 0x8", 0x42800008},
    {"blr", 0x4e800020},
    {"blrl", 0x4e800021},
    {"bctr", 0x4e800420},
    {"bctrl", 0x4e800421},
    {"bclr 12, 2", 0x4d820020},
    {"bcctr 4, 6", 0x4c860420},
    {"crand 1, 2, 3", 0x4c221a02},
    {"crandc 1, 2, 3", 0x4c221902},
    {"creqv 6, 6, 6", 0x4cc63242},
    {"crnand 1, 2, 3", 0x4c2219c2},
    {"crnor 1, 2, 3", 0x4c221842},
    {"cror 31, 0, 1", 0x4fe00b82},
    {"crorc 1, 2, 3", 0x4c221b42},
    {"crxor 6, 6, 6", 0x4cc63182},
    {"mcrf cr1, cr7", 0x4c9c0000},
    {"mfcr r3", 0x7c600026},
    {"mtcrf 0xff, r3", 0x7c6ff120},
    {"mfspr r3, xer", 0x7c6102a6},
    {"mflr r3", 0x7c6802a6},
    {"mfctr r3", 0x7c6902a6},
    {"mfspr r3, vrsave", 0x7c6042a6},
    {"mtspr xer, r3", 0x7c6103a6},
    {"mtlr r3", 0x7c6803a6},
    {"mtctr r3", 0x7c6903a6},
    {"mftb r3, tb", 0x7c6c42e6},
    {"mftb r3, tbu", 0x7c6d42e6},
    {"tw 31, r0, r0", 0x7fe00008},
    {"twi 4, r3, 0", 0x0c830000},
    {"td 4, r3, r4", 0x7c832088},
    {"tdi 16, r3, -1", 0x0a03ffff},
    {"sc", 0x44000002},
    {"sync", 0x7c0004ac},
    {"eieio", 0x7c0006ac},
    {"isync", 0x4c00012c},
    {"lbz r3, 0(r1)", 0x88610000},
    {"lbzu r3, 1(r4)", 0x8c640001},
    {"lbzx r3, r4, r5", 0x7c6428ae},
    {"lbzux r3, r4, r5", 0x7c6428ee},
    {"lhz r3, -2(r1)", 0xa061fffe},
    {"lhzu r3, 2(r4)", 0xa4640002},
    {"lhzx r3, r4, r5", 0x7c642a2e},
    {"lhzux r3, r4, r5", 0x7c642a6e},
    {"lha r3, 4(r1)", 0xa8610004},
    {"lhau r3, -4(r4)", 0xac64fffc},
    {"lhax r3, r4, r5", 0x7c642aae},
    {"lhaux r3, r4, r5", 0x7c642aee},
    {"lhbrx r3, r4, r5", 0x7c642e2c},
    {"lwz r3, 8(r1)", 0x80610008},
    {"lwzu r3, 4(r4)", 0x84640004},
    {"lwzx r3, r4, r5", 0x7c64282e},
    {"lwzux r3, r4, r5", 0x7c64286e},
    {"lwa r3, 8(r1)", 0xe861000a},
    {"lwax r3, r4, r5", 0x7c642aaa},
    {"lwaux r3, r4, r5", 0x7c642aea},
    {"lwbrx r3, r4, r5", 0x7c642c2c},
    {"ld r3, -16(r1)", 0xe861fff0},
    {"ldu r3, 8(r4)", 0xe8640009},
    {"ldx r3, r4, r5", 0x7c64282a},
    {"ldux r3, r4, r5", 0x7c64286a},
    {"stb r3, 0(r1)", 0x98610000},
    {"stbu r3, 1(r4)", 0x9c640001},
    {"stbx r3, r4, r5", 0x7c6429ae},
    {"stbux r3, r4, r5", 0x7c6429ee},
    {"sth r3, 2(r1)", 0xb0610002},
    {"sthu r3, -2(r4)", 0xb464fffe},
    {"sthx r3, r4, r5", 0x7c642b2e},
    {"sthux r3, r4, r5", 0x7c642b6e},
    {"sthbrx r3, r4, r5", 0x7c642f2c},
    {"stw r3, 4(r1)", 0x90610004},
    {"stwu r1, -64(r1)", 0x9421ffc0},
    {"stwx r3, r4, r5", 0x7c64292e},
    {"stwux r3, r4, r5", 0x7c64296e},
    {"stwbrx r3, r4, r5", 0x7c642d2c},
    {"std r31, -8(r1)", 0xfbe1fff8},
    {"stdu r1, -128(r1)", 0xf821ff81},
    {"stdx r3, r4, r5", 0x7c64292a},
    {"stdux r3, r4, r5", 0x7c64296a},
    {"lmw r29, -12(r1)", 0xbba1fff4},
    {"stmw r29, -12(r1)", 0xbfa1fff4},
    {"lwarx r3, r4, r5", 0x7c642828},
    {"ldarx r3, r4, r5", 0x7c6428a8},
    {"stwcx. r3, r4, r5", 0x7c64292d},
    {"stdcx. r3, r4, r5", 0x7c6429ad},
    {"dcbz r3, r4", 0x7c0327ec},
    {"dcbt r3, r4", 0x7c03222c},
    {"dcbtst r3, r4", 0x7c0321ec},
    {"dcbf r3, r4", 0x7c0320ac},
    {"dcbst r3, r4", 0x7c03206c},
    {"icbi r3, r4", 0x7c0327ac},
    {"lfs f1, 8(r1)", 0xc0210008},
    {"lfsu f2, -4(r4)", 0xc444fffc},
    {"lfsx f3, r4, r5", 0x7c642c2e},
    {"lfsux f4, r4, r5", 0x7c842c6e},
    {"stfs f8, 12(r1)", 0xd101000c},
    {"stfsu f9, 4(r4)", 0xd5240004},
    {"stfsx f10, r4, r5", 0x7d442d2e},
    {"stfsux f11, r4, r5", 0x7d642d6e},
    {"lfd f1, 8(r1)", 0xc8210008},
    {"lfdu f2, -8(r4)", 0xcc44fff8},
    {"lfdx f3, r4, r5", 0x7c642cae},
    {"lfdux f4, r4, r5", 0x7c842cee},
    {"stfd f31, -8(r1)", 0xdbe1fff8},
    {"stfdu f5, 16(r4)", 0xdca40010},
    {"stfdx f6, r4, r5", 0x7cc42dae},
    {"stfdux f7, r4, r5", 0x7ce42dee},
    // Emulator-defined, so LLVM doesn't know it: primary opcode 1, then the index.
    {"hle 5", 0x04000005},
};
// clang-format on

} // namespace

TEST_CASE("the assembler matches LLVM's encodings") {
    for (const Encoding& e : k_encodings) {
        INFO(e.text);
        CHECK(assemble(e.text) == e.raw);
    }
}

TEST_CASE("the disassembler prints LLVM's encodings back as the same text") {
    for (const Encoding& e : k_encodings) {
        INFO(e.text);
        CHECK(disassemble(e.raw, 0) == e.text);
    }
}

TEST_CASE("the reference encodings cover every opcode in the tables") {
    std::set<const OpcodeInfo*> covered;
    for (const Encoding& e : k_encodings) {
        covered.insert(decoder().decode(e.raw));
    }
    for (const OpcodeInfo* info : decoder().all()) {
        INFO(info->mnemonic);
        CHECK(covered.contains(info));
    }
}

TEST_CASE("branch targets are relative to the instruction's address") {
    CHECK(assemble("b 0x10100", 0x10000) == assemble("b 0x100", 0));
    CHECK(disassemble(assemble("bl 0x10000", 0x10010), 0x10010) == "bl 0x10000");
    CHECK(disassemble(assemble("bne cr1, 0xff00", 0x10000), 0x10000) == "bne cr1, 0xff00");
}

TEST_CASE("unknown instructions disassemble as data") {
    CHECK(disassemble(0, 0) == ".long 0x00000000");
}

TEST_CASE("the assembler reports bad input") {
    CHECK_THROWS_WITH(assemble("frobnicate r1"), ContainsSubstring("unknown instruction"));
    CHECK_THROWS_WITH(assemble("add r3, r4"), ContainsSubstring("expects operands"));
    CHECK_THROWS_WITH(assemble("add r3, r4, r32"), ContainsSubstring("no such register"));
    CHECK_THROWS_WITH(assemble("addi r3, r4, 40000"), ContainsSubstring("does not fit"));
    CHECK_THROWS_WITH(assemble("ld r3, 2(r1)"), ContainsSubstring("multiple of 4"));
    CHECK_THROWS_WITH(assemble("lwz r3, r1"), ContainsSubstring("offset(rA)"));
    CHECK_THROWS_WITH(assemble("b 0x102"), ContainsSubstring("multiple of 4"));
}
