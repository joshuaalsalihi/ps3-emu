#include "cpu/ppu/opcodes.h"
#include "cpu/ppu/operands.h"

#include <catch2/catch_test_macros.hpp>

using namespace ppu;

TEST_CASE("every opcode table entry decodes to itself") {
    // Catches overlapping entries: an earlier entry claiming another's encoding.
    for (const OpcodeInfo* info : decoder().all()) {
        INFO(info->mnemonic);
        CHECK(decoder().decode(info->match()) == info);
    }
}

TEST_CASE("every operand layout parses") {
    for (const OpcodeInfo* info : decoder().all()) {
        INFO(info->mnemonic);
        CHECK_NOTHROW(parse_operand_layout(info->operands));
    }
}

TEST_CASE("decoding ignores operand fields but not opcode fields") {
    const OpcodeInfo* add = decoder().decode(0x7c642a14); // add r3, r4, r5
    REQUIRE(add != nullptr);
    CHECK(add->mnemonic == "add");
    CHECK(decoder().decode(0x7c642a15) == add); // add. (Rc = 1)
    CHECK(decoder().decode(0x7c642e14) == add); // addo (OE = 1)

    CHECK(decoder().decode(0x44000000) == nullptr); // sc requires bit 30
    CHECK(decoder().decode(0x7c64292c) == nullptr); // stwcx. requires bit 31
}
