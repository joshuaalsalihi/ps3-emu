// Translating shader IR to Metal Shading Language. The text is checked here; whether Metal
// accepts it and draws the same as the reference is checked in metal_backend_test.cpp.

#include "gpu/msl.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace rsx;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

vertex::Instruction move_to_output(u32 input, u32 output) {
    vertex::Instruction instruction;
    instruction.vector_opcode = 0x01;
    instruction.input = input;
    instruction.sources[0].type = vertex::SourceType::input;
    instruction.vector_to_output = true;
    instruction.vector_mask = 0xf;
    instruction.output = output;
    return instruction;
}

} // namespace

TEST_CASE("a pass-through program translates to MSL") {
    fragment::Instruction move;
    move.opcode = 0x01;
    move.input = fragment::k_input_color0;
    move.sources[0].type = fragment::SourceType::input;
    move.mask = 0xf;
    move.end = true;

    const gpu::MslTranslation msl =
        gpu::translate_to_msl({move_to_output(0, 0), move_to_output(3, 1)}, {move}, false);
    REQUIRE_FALSE(msl.error);
    CHECK(contains(msl.source, "vertex Varyings vertex_main("));
    CHECK(contains(msl.source, "fragment float4 fragment_main("));
    CHECK(contains(msl.source, "float4 a = v[3].xyzw;"));
    CHECK(contains(msl.source, "o[1].xyzw = (vector).xyzw;"));
    CHECK(contains(msl.source, "f[1] = in.o1;"));
    CHECK(contains(msl.source, "r[0].xyzw = (a).xyzw;"));
    CHECK(contains(msl.source, "float4 o16 [[user(o16)]];"));
}

TEST_CASE("modifiers, masks, saturation and flat shading appear in the MSL") {
    vertex::Instruction instruction = move_to_output(0, 7);
    instruction.vector_opcode = 0x03; // ADD: sources 0 and 2
    instruction.sources[0].swizzle = {3, 2, 1, 0};
    instruction.sources[0].negate = true;
    instruction.sources[2].type = vertex::SourceType::constant;
    instruction.sources[2].absolute = true;
    instruction.constant = 5;
    instruction.vector_mask = 0b0101;
    instruction.saturate = true;

    const gpu::MslTranslation msl = gpu::translate_to_msl({instruction}, {}, true);
    REQUIRE_FALSE(msl.error);
    CHECK(contains(msl.source, "float4 a = -v[0].wzyx;"));
    CHECK(contains(msl.source, "float4 c = abs(constants[5].xyzw);"));
    CHECK(contains(msl.source, "float4 vector = a + c;"));
    CHECK(contains(msl.source, "o[7].xz = clamp(vector, 0.0, 1.0).xz;"));
    CHECK(contains(msl.source, "[[user(o1), flat]]"));
}

TEST_CASE("instructions MSL can't express yet are reported") {
    vertex::Instruction instruction = move_to_output(0, 0);
    instruction.vector_opcode = 0x0d; // ARL
    const gpu::MslTranslation msl = gpu::translate_to_msl({instruction}, {}, false);
    REQUIRE(msl.error);
    CHECK(*msl.error == "vertex vector opcode 0xd can't be translated to Metal yet");
}
