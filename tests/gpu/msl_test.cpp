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

    rsx::DrawRequest request;
    request.vertex_program.instructions = {move_to_output(0, 0), move_to_output(3, 1)};
    request.fragment_program.instructions = {move};
    const gpu::MslTranslation msl = gpu::translate_to_msl(request);
    REQUIRE_FALSE(msl.error);
    CHECK(contains(msl.source, "vertex Varyings vertex_main("));
    CHECK(contains(msl.source, "fragment float4 fragment_main("));
    CHECK(contains(msl.source, "float4 a = v[3].xyzw;"));
    CHECK(contains(msl.source, "o[1].xyzw = (vector).xyzw;"));
    CHECK(contains(msl.source, "f[1] = in.o1;"));
    CHECK(contains(msl.source, "float4 value = float4(a);"));
    CHECK(contains(msl.source, "if (pass.x) { r[0].x = value.x; }"));
    CHECK(contains(msl.source, "return r[0];"));
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

    rsx::DrawRequest request;
    request.vertex_program.instructions = {instruction};
    request.flat_shading = true;
    const gpu::MslTranslation msl = gpu::translate_to_msl(request);
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
    rsx::DrawRequest request;
    request.vertex_program.instructions = {instruction};
    const gpu::MslTranslation msl = gpu::translate_to_msl(request);
    REQUIRE(msl.error);
    CHECK(*msl.error == "vertex vector opcode 0xd can't be translated to Metal yet");
}

TEST_CASE("texture lookups and the alpha test appear in the MSL") {
    fragment::Instruction lookup;
    lookup.opcode = 0x18; // TXP
    lookup.texture_unit = 2;
    lookup.input = fragment::k_input_texcoord0;
    lookup.sources[0].type = fragment::SourceType::input;
    lookup.mask = 0xf;
    lookup.end = true;

    rsx::DrawRequest request;
    request.fragment_program.instructions = {lookup};
    CHECK(gpu::translate_to_msl(request).error ==
          "a missing texture can't be translated to Metal yet");

    request.textures[2] = rsx::Texture{};
    request.textures[2]->lod_bias = -128; // -0.5
    request.alpha_test = rsx::AlphaTest{rsx::Compare::greater_equal, 0x40};
    const gpu::MslTranslation msl = gpu::translate_to_msl(request);
    REQUIRE_FALSE(msl.error);
    CHECK(contains(msl.source, "texture2d<float> t2 [[texture(2)]], sampler s2 [[sampler(2)]]"));
    CHECK(contains(msl.source, "t2.sample(s2, a.xy / a.w, bias(-0x80p-8))"));
    CHECK(contains(msl.source, "if (!(alpha >= 64u)) discard_fragment();"));
}

TEST_CASE("half registers, precision, the condition code and KIL appear in the MSL") {
    fragment::Instruction move; // MOVHC H2.x, COL0 at fp16 precision, where the code is > 0
    move.opcode = 0x01;
    move.input = fragment::k_input_color0;
    move.sources[0].type = fragment::SourceType::input;
    move.mask = 0b0001;
    move.destination = 2;
    move.destination_half = true;
    move.precision = fragment::Precision::fp16;
    move.set_condition = true;
    move.condition = fragment::k_condition_greater;
    fragment::Instruction kill; // KIL LT
    kill.opcode = 0x12;
    kill.condition = fragment::k_condition_less;
    kill.end = true;

    rsx::DrawRequest request;
    request.fragment_program.instructions = {move, kill};
    request.fragment_program.half_output = true;
    const gpu::MslTranslation msl = gpu::translate_to_msl(request);
    REQUIRE_FALSE(msl.error);
    CHECK(contains(msl.source, "bool4 pass = (cc.xyzw > 0.0);"));
    CHECK(contains(msl.source, "float4 value = float4(half4(float4(a)));"));
    CHECK(contains(msl.source, "if (pass.x) { cc.x = value.x; h[2].x = float(half(value.x)); }"));
    CHECK(contains(msl.source, "if (any(pass)) discard_fragment();"));
    CHECK(contains(msl.source, "return h[0];"));
}
