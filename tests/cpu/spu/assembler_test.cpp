// SPU assembler and disassembler, checked against encodings from the PS3 toolchain's SPU
// assembler (spu-as, then spu-objdump -d), an independent implementation. LLVM has no SPU
// backend any more.

#include "cpu/spu/assembler.h"
#include "cpu/spu/disassembler.h"
#include "cpu/spu/opcodes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <set>
#include <string_view>

using namespace spu;
using Catch::Matchers::ContainsSubstring;

namespace {

struct Encoding {
    std::string_view text; // as our disassembler prints it, at address 0
    u32 raw;               // as spu-as encodes it
};

// clang-format off
constexpr Encoding k_encodings[] = {
    {"lqd $3, 16($1)", 0x34004083},
    {"lqd $3, -32($1)", 0x34ff8083},
    {"lqx $3, $4, $5", 0x38814203},
    {"lqa $3, 0x100", 0x30802003},
    {"lqr $3, 0x40", 0x33800803},
    {"stqd $3, 16($1)", 0x24004083},
    {"stqx $3, $4, $5", 0x28814203},
    {"stqa $3, 0x100", 0x20802003},
    {"stqr $3, 0x40", 0x23800803},
    {"cbd $3, 1($4)", 0x3e804203},
    {"cbx $3, $4, $5", 0x3a814203},
    {"chd $3, 2($4)", 0x3ea08203},
    {"chx $3, $4, $5", 0x3aa14203},
    {"cwd $3, 4($4)", 0x3ec10203},
    {"cwx $3, $4, $5", 0x3ac14203},
    {"cdd $3, 8($4)", 0x3ee20203},
    {"cdx $3, $4, $5", 0x3ae14203},
    {"ilh $3, 0x1234", 0x41891a03},
    {"ilhu $3, 0x8000", 0x41400003},
    {"il $3, -1", 0x40ffff83},
    {"ila $3, 0x3ffff", 0x43ffff83},
    {"iohl $3, 0xffff", 0x60ffff83},
    {"fsmbi $3, 0xf0f0", 0x32f87803},
    {"ah $3, $4, $5", 0x19014203},
    {"ahi $3, $4, -2", 0x1dff8203},
    {"a $3, $4, $5", 0x18014203},
    {"ai $3, $4, 100", 0x1c190203},
    {"sfh $3, $4, $5", 0x09014203},
    {"sfhi $3, $4, 7", 0x0d01c203},
    {"sf $3, $4, $5", 0x08014203},
    {"sfi $3, $4, -512", 0x0c800203},
    {"addx $3, $4, $5", 0x68014203},
    {"cg $3, $4, $5", 0x18414203},
    {"cgx $3, $4, $5", 0x68414203},
    {"sfx $3, $4, $5", 0x68214203},
    {"bg $3, $4, $5", 0x08414203},
    {"bgx $3, $4, $5", 0x68614203},
    {"mpy $3, $4, $5", 0x78814203},
    {"mpyu $3, $4, $5", 0x79814203},
    {"mpyi $3, $4, -3", 0x74ff4203},
    {"mpyui $3, $4, 511", 0x757fc203},
    {"mpya $3, $4, $5, $6", 0xc0614206},
    {"mpyh $3, $4, $5", 0x78a14203},
    {"mpys $3, $4, $5", 0x78e14203},
    {"mpyhh $3, $4, $5", 0x78c14203},
    {"mpyhha $3, $4, $5", 0x68c14203},
    {"mpyhhu $3, $4, $5", 0x79c14203},
    {"mpyhhau $3, $4, $5", 0x69c14203},
    {"clz $3, $4", 0x54a00203},
    {"cntb $3, $4", 0x56800203},
    {"fsmb $3, $4", 0x36c00203},
    {"fsmh $3, $4", 0x36a00203},
    {"fsm $3, $4", 0x36800203},
    {"gbb $3, $4", 0x36400203},
    {"gbh $3, $4", 0x36200203},
    {"gb $3, $4", 0x36000203},
    {"avgb $3, $4, $5", 0x1a614203},
    {"absdb $3, $4, $5", 0x0a614203},
    {"sumb $3, $4, $5", 0x4a614203},
    {"xsbh $3, $4", 0x56c00203},
    {"xshw $3, $4", 0x55c00203},
    {"xswd $3, $4", 0x54c00203},
    {"and $3, $4, $5", 0x18214203},
    {"andc $3, $4, $5", 0x58214203},
    {"andbi $3, $4, 255", 0x163fc203},
    {"andhi $3, $4, -1", 0x15ffc203},
    {"andi $3, $4, 511", 0x147fc203},
    {"or $3, $4, $5", 0x08214203},
    {"orc $3, $4, $5", 0x59214203},
    {"orbi $3, $4, 15", 0x0603c203},
    {"orhi $3, $4, -16", 0x05fc0203},
    {"ori $3, $4, 0", 0x04000203},
    {"orx $3, $4", 0x3e000203},
    {"xor $3, $4, $5", 0x48214203},
    {"xorbi $3, $4, 1", 0x46004203},
    {"xorhi $3, $4, 2", 0x45008203},
    {"xori $3, $4, -3", 0x44ff4203},
    {"nand $3, $4, $5", 0x19214203},
    {"nor $3, $4, $5", 0x09214203},
    {"eqv $3, $4, $5", 0x49214203},
    {"selb $3, $4, $5, $6", 0x80614206},
    {"shufb $3, $4, $5, $6", 0xb0614206},
    {"shlh $3, $4, $5", 0x0be14203},
    {"shlhi $3, $4, 3", 0x0fe0c203},
    {"shl $3, $4, $5", 0x0b614203},
    {"shli $3, $4, 31", 0x0f67c203},
    {"shlqbi $3, $4, $5", 0x3b614203},
    {"shlqbii $3, $4, 7", 0x3f61c203},
    {"shlqby $3, $4, $5", 0x3be14203},
    {"shlqbyi $3, $4, 15", 0x3fe3c203},
    {"shlqbybi $3, $4, $5", 0x39e14203},
    {"roth $3, $4, $5", 0x0b814203},
    {"rothi $3, $4, -1", 0x0f9fc203},
    {"rot $3, $4, $5", 0x0b014203},
    {"roti $3, $4, 8", 0x0f020203},
    {"rotqby $3, $4, $5", 0x3b814203},
    {"rotqbyi $3, $4, 4", 0x3f810203},
    {"rotqbybi $3, $4, $5", 0x39814203},
    {"rotqbi $3, $4, $5", 0x3b014203},
    {"rotqbii $3, $4, 5", 0x3f014203},
    {"rothm $3, $4, $5", 0x0ba14203},
    {"rothmi $3, $4, -4", 0x0fbf0203},
    {"rotm $3, $4, $5", 0x0b214203},
    {"rotmi $3, $4, -31", 0x0f384203},
    {"rotqmby $3, $4, $5", 0x3ba14203},
    {"rotqmbyi $3, $4, -8", 0x3fbe0203},
    {"rotqmbybi $3, $4, $5", 0x39a14203},
    {"rotqmbi $3, $4, $5", 0x3b214203},
    {"rotqmbii $3, $4, -7", 0x3f3e4203},
    {"rotmah $3, $4, $5", 0x0bc14203},
    {"rotmahi $3, $4, -15", 0x0fdc4203},
    {"rotma $3, $4, $5", 0x0b414203},
    {"rotmai $3, $4, -1", 0x0f5fc203},
    {"heq $4, $5", 0x7b014200},
    {"heqi $4, -1", 0x7fffc200},
    {"hgt $4, $5", 0x4b014200},
    {"hgti $4, 10", 0x4f028200},
    {"hlgt $4, $5", 0x5b014200},
    {"hlgti $4, 10", 0x5f028200},
    {"ceqb $3, $4, $5", 0x7a014203},
    {"ceqbi $3, $4, -1", 0x7effc203},
    {"ceqh $3, $4, $5", 0x79014203},
    {"ceqhi $3, $4, 100", 0x7d190203},
    {"ceq $3, $4, $5", 0x78014203},
    {"ceqi $3, $4, 0", 0x7c000203},
    {"cgtb $3, $4, $5", 0x4a014203},
    {"cgtbi $3, $4, 5", 0x4e014203},
    {"cgth $3, $4, $5", 0x49014203},
    {"cgthi $3, $4, -5", 0x4dfec203},
    {"cgt $3, $4, $5", 0x48014203},
    {"cgti $3, $4, -1", 0x4cffc203},
    {"clgtb $3, $4, $5", 0x5a014203},
    {"clgtbi $3, $4, 5", 0x5e014203},
    {"clgth $3, $4, $5", 0x59014203},
    {"clgthi $3, $4, 5", 0x5d014203},
    {"clgt $3, $4, $5", 0x58014203},
    {"clgti $3, $4, 5", 0x5c014203},
    {"br 0x40", 0x32000800},
    {"br 0x3fff8", 0x327fff00},
    {"bra 0x1000", 0x30020000},
    {"brsl $0, 0x80", 0x33001000},
    {"brasl $0, 0x200", 0x31004000},
    {"bi $0", 0x35000000},
    {"bid $0", 0x35080000},
    {"bie $0", 0x35040000},
    {"iret $0", 0x35400000},
    {"bisled $0, $4", 0x35600200},
    {"bisl $0, $4", 0x35200200},
    {"brnz $3, 0x10", 0x21000203},
    {"brz $3, 0x10", 0x20000203},
    {"brhnz $3, 0x10", 0x23000203},
    {"brhz $3, 0x10", 0x22000203},
    {"biz $3, $4", 0x25000203},
    {"binz $3, $4", 0x25200203},
    {"bihz $3, $4", 0x25400203},
    {"bihnz $3, $4", 0x25600203},
    {"hbr 0x20, $4", 0x35800208},
    {"hbra 0x20, 0x400", 0x10008008},
    {"hbrr 0x20, 0x100", 0x12002008},
    {"hbrp", 0x35900000},
    {"fa $3, $4, $5", 0x58814203},
    {"dfa $3, $4, $5", 0x59814203},
    {"fs $3, $4, $5", 0x58a14203},
    {"dfs $3, $4, $5", 0x59a14203},
    {"fm $3, $4, $5", 0x58c14203},
    {"dfm $3, $4, $5", 0x59c14203},
    {"fma $3, $4, $5, $6", 0xe0614206},
    {"dfma $3, $4, $5", 0x6b814203},
    {"fnms $3, $4, $5, $6", 0xd0614206},
    {"dfnms $3, $4, $5", 0x6bc14203},
    {"fms $3, $4, $5, $6", 0xf0614206},
    {"dfms $3, $4, $5", 0x6ba14203},
    {"dfnma $3, $4, $5", 0x6be14203},
    {"frest $3, $4", 0x37000203},
    {"frsqest $3, $4", 0x37200203},
    {"fi $3, $4, $5", 0x7a814203},
    {"csflt $3, $4, 0", 0x76a6c203},
    {"cflts $3, $4, 10", 0x7628c203},
    {"cuflt $3, $4, 127", 0x76c70203},
    {"cfltu $3, $4, 32", 0x76634203},
    {"frds $3, $4", 0x77200203},
    {"fesd $3, $4", 0x77000203},
    {"fceq $3, $4, $5", 0x78414203},
    {"fcmeq $3, $4, $5", 0x79414203},
    {"fcgt $3, $4, $5", 0x58414203},
    {"fcmgt $3, $4, $5", 0x59414203},
    {"fscrwr $4", 0x77400200},
    {"fscrrd $3", 0x73000003},
    {"stop 0x2000", 0x00002000},
    {"stop 0x3fff", 0x00003fff},
    {"stopd $3, $4, $5", 0x28014203},
    {"lnop", 0x00200000},
    {"nop $0", 0x40200000},
    {"nop $127", 0x4020007f},
    {"sync", 0x00400000},
    {"syncc", 0x00500000},
    {"dsync", 0x00600000},
    {"mfspr $3, $sp0", 0x01800003},
    {"mtspr $sp0, $3", 0x21800003},
    {"rdch $3, $ch29", 0x01a00e83},
    {"rchcnt $3, $ch29", 0x01e00e83},
    {"wrch $ch28, $3", 0x21a00e03},
    {"iretd $0", 0x35480000},
    {"bisledd $0, $4", 0x35680200},
    {"bisle $0, $4", 0x35240200},
    {"bisld $0, $4", 0x35280200},
    {"bize $3, $4", 0x25040203},
    {"binzd $3, $4", 0x25280203},
    {"bihze $3, $4", 0x25440203},
    {"bihnzd $3, $4", 0x25680203},
    {"nop $5", 0x40200005},
};
// clang-format on

} // namespace

TEST_CASE("the SPU assembler matches spu-as's encodings") {
    for (const Encoding& e : k_encodings) {
        INFO(e.text);
        CHECK(assemble(e.text) == e.raw);
    }
}

TEST_CASE("the SPU disassembler prints spu-as's encodings back as the same text") {
    for (const Encoding& e : k_encodings) {
        INFO(e.text);
        CHECK(disassemble(e.raw, 0) == e.text);
    }
}

TEST_CASE("the SPU reference encodings cover every opcode in the tables") {
    std::set<const OpcodeInfo*> covered;
    for (const Encoding& e : k_encodings) {
        covered.insert(decoder().decode(e.raw));
    }
    for (const OpcodeInfo* info : decoder().all()) {
        INFO(info->mnemonic);
        CHECK(covered.contains(info));
    }
}

TEST_CASE("SPU branch targets are relative to the instruction and wrap around local store") {
    CHECK(assemble("br 0x100", 0x40) == assemble("br 0xc0", 0));
    CHECK(disassemble(assemble("brsl $0, 0x1000", 0x2000), 0x2000) == "brsl $0, 0x1000");
    CHECK(disassemble(assemble("br 0x3fff8", 0), 0) == "br 0x3fff8");
    CHECK(disassemble(assemble("hbrr 0x3fffc, 0x10", 0x10), 0x10) == "hbrr 0x3fffc, 0x10");
}

TEST_CASE("the SPU assembler accepts the ABI register names") {
    CHECK(assemble("bi $lr") == assemble("bi $0"));
    CHECK(assemble("lqd $3, 0($sp)") == assemble("lqd $3, 0($1)"));
}

TEST_CASE("the optional double-precision compares are illegal on the Cell") {
    CHECK(decoder().decode(0x78614203) == nullptr); // dfceq $3, $4, $5 (spu-as)
    CHECK(decoder().decode(0x77e04203) == nullptr); // dftsv $3, $4, 1 (spu-as)
    CHECK(disassemble(0x78614203, 0) == ".long 0x78614203");
}

TEST_CASE("the SPU assembler reports bad input") {
    CHECK_THROWS_WITH(assemble("frobnicate $1"), ContainsSubstring("unknown instruction"));
    CHECK_THROWS_WITH(assemble("a $3, $4"), ContainsSubstring("expects operands"));
    CHECK_THROWS_WITH(assemble("a $3, $4, $128"), ContainsSubstring("no such register"));
    CHECK_THROWS_WITH(assemble("ai $3, $4, 512"), ContainsSubstring("does not fit"));
    CHECK_THROWS_WITH(assemble("lqd $3, 8($1)"), ContainsSubstring("multiple of 16"));
    CHECK_THROWS_WITH(assemble("lqd $3, $1"), ContainsSubstring("offset(RA)"));
    CHECK_THROWS_WITH(assemble("br 0x102"), ContainsSubstring("multiple of 4"));
    CHECK_THROWS_WITH(assemble("hbrr 0x800, 0"), ContainsSubstring("too far"));
    CHECK_THROWS_WITH(assemble("rdch $3, $4"), ContainsSubstring("$ch"));
}
