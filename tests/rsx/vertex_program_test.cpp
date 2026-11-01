// Vertex programs. The anchors are real instructions from the PS3 toolchain's cgcomp; the other
// tests build instructions field by field, following nouveau's layout.

#include "rsx/vertex_program.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <vector>

using namespace rsx;
using namespace rsx::vertex;

namespace {

shader_float::Float f(float value) {
    return std::bit_cast<u32>(value);
}

Vec4 v(float x, float y, float z, float w) {
    return {f(x), f(y), f(z), f(w)};
}

// A source field: register type, temp, identity swizzle unless given, negate.
constexpr u32 k_identity = 0x1b; // x = 0, y = 1, z = 2, w = 3, x in the top two bits
u32 source(SourceType type, u32 temp = 0, u32 swizzle = k_identity, bool negate = false) {
    return static_cast<u32>(type) | temp << 2 | swizzle << 8 | (negate ? 1u << 16 : 0);
}

// Builds an instruction; write masks list x in their top bit (0x8 = x only).
struct Fields {
    u32 vector_op = 0;
    u32 scalar_op = 0;
    u32 input = 0;
    u32 constant = 0;
    std::array<u32, 3> sources{};
    u32 vector_temp = 0x3f; // none
    bool vector_to_output = false;
    u32 vector_mask = 0;
    u32 scalar_temp = 0x1f; // none
    bool scalar_to_output = false;
    u32 scalar_mask = 0;
    u32 output = 0;
    bool saturate = false;
    bool last = false;
};

std::array<u32, 4> encode(const Fields& in) {
    const u32 condition_true = 7 << 10 | k_identity << 2;
    return {
        (in.vector_to_output ? 1u << 30 : 0) | (in.saturate ? 1u << 26 : 0) | in.vector_temp << 15 |
            condition_true,
        in.scalar_op << 27 | in.vector_op << 22 | in.constant << 12 | in.input << 8 |
            in.sources[0] >> 9,
        (in.sources[0] & 0x1ff) << 23 | in.sources[1] << 6 | in.sources[2] >> 11,
        (in.sources[2] & 0x7ff) << 21 | in.scalar_mask << 17 | in.vector_mask << 13 |
            (in.scalar_to_output ? 1u << 12 : 0) | in.scalar_temp << 7 | in.output << 2 |
            (in.last ? 1u : 0),
    };
}

// Runs instructions (the last one marked last) on `inputs`, with constant 0 = `constant`.
Result run_program(std::vector<Fields> program, const std::array<Vec4, k_input_count>& inputs,
                   const Vec4& constant = {}) {
    program.back().last = true;
    std::vector<u32> memory;
    for (const Fields& fields : program) {
        const auto words = encode(fields);
        memory.insert(memory.end(), words.begin(), words.end());
    }
    Program decoded;
    decoded.instructions = *decode_program(memory, 0);
    decoded.constants[0] = constant;
    return run(decoded, inputs);
}

constexpr u32 k_xyzw = 0xf;

} // namespace

TEST_CASE("cgcomp's MOVs to outputs decode as such") {
    // rsx_triangle.vcg: MOV result.position, vertex.position; MOV result.color, vertex.color.
    const std::array<u32, 4> position = {0x401f9c6c, 0x0040000d, 0x8106c083, 0x6041ff80};
    const Instruction mov = decode(position);
    CHECK(mov.vector_opcode == 1);
    CHECK(mov.scalar_opcode == 0);
    CHECK(mov.input == 0);
    CHECK(mov.sources[0].type == SourceType::input);
    CHECK(mov.sources[0].swizzle == std::array<u8, 4>{0, 1, 2, 3});
    CHECK(mov.vector_to_output);
    CHECK(mov.output == k_output_position);
    CHECK(mov.vector_mask == 0xf);
    CHECK_FALSE(mov.vector_temp);
    CHECK_FALSE(mov.condition_test);
    CHECK_FALSE(mov.last);

    const std::array<u32, 4> color = {0x401f9c6c, 0x0040030d, 0x8106c083, 0x6041ff85};
    const Instruction last = decode(color);
    CHECK(last.input == 3);
    CHECK(last.output == k_output_color0);
    CHECK(last.last);
}

TEST_CASE("a program runs up to its last instruction") {
    const std::vector<u32> memory = {0x401f9c6c, 0x0040000d, 0x8106c083, 0x6041ff80,
                                     0x401f9c6c, 0x0040030d, 0x8106c083, 0x6041ff85};
    Program program;
    program.instructions = *decode_program(memory, 0);
    CHECK(program.instructions.size() == 2);
    CHECK(decode_program(memory, 1)->size() == 1);
    CHECK_FALSE(decode_program(std::span(memory).first(4), 0)); // no last instruction

    std::array<Vec4, k_input_count> inputs{};
    inputs[0] = v(1, 2, 3, 4);
    inputs[3] = v(0.5f, 0.25f, 0, 1);
    const Result result = run(program, inputs);
    CHECK(result.outputs[k_output_position] == v(1, 2, 3, 4));
    CHECK(result.outputs[k_output_color0] == v(0.5f, 0.25f, 0, 1));
}

TEST_CASE("vector operations read sources 0 and 1, and MAD and ADD source 2") {
    std::array<Vec4, k_input_count> inputs{};
    inputs[0] = v(1, 2, 3, 4);
    const Vec4 constant = v(2, 3, 4, 0.5f);
    const u32 in = source(SourceType::input);
    const u32 c = source(SourceType::constant);
    const u32 t = source(SourceType::temp, 1);

    auto to_output = [](Fields fields) {
        fields.vector_to_output = true;
        fields.vector_mask = k_xyzw;
        return fields;
    };
    CHECK(run_program({to_output({.vector_op = 0x02, .sources = {in, c, 0}})}, inputs, constant)
              .outputs[0] == v(2, 6, 12, 2));
    CHECK(run_program({to_output({.vector_op = 0x03, .sources = {in, 0, c}})}, inputs, constant)
              .outputs[0] == v(3, 5, 7, 4.5f));
    CHECK(run_program({to_output({.vector_op = 0x04, .sources = {in, c, in}})}, inputs, constant)
              .outputs[0] == v(3, 8, 15, 6));
    CHECK(run_program({to_output({.vector_op = 0x07, .sources = {in, c, 0}})}, inputs, constant)
              .outputs[0] == v(22, 22, 22, 22));
    CHECK(run_program({to_output({.vector_op = 0x05, .sources = {in, c, 0}})}, inputs, constant)
              .outputs[0] == v(20, 20, 20, 20));

    // Through a temp: t1 = in * c, then out = t1 + c.
    const Result chained = run_program(
        {{.vector_op = 0x02, .sources = {in, c, 0}, .vector_temp = 1, .vector_mask = k_xyzw},
         to_output({.vector_op = 0x03, .sources = {t, 0, c}})},
        inputs, constant);
    CHECK(chained.outputs[0] == v(4, 9, 16, 2.5f));
}

TEST_CASE("sources can be swizzled, negated and made absolute") {
    std::array<Vec4, k_input_count> inputs{};
    inputs[0] = v(1, -2, 3, 4);
    // wzyx = w in the top two bits... x's selector comes first: 3, 2, 1, 0.
    const u32 reversed = source(SourceType::input, 0, 0b11'10'01'00, true);
    Fields fields{.vector_op = 0x01,
                  .sources = {reversed, 0, 0},
                  .vector_to_output = true,
                  .vector_mask = k_xyzw};
    CHECK(run_program({fields}, inputs).outputs[0] == v(-4, -3, 2, -1));

    auto words = encode(fields);
    words[0] |= 1u << 21; // source 0 absolute
    words[3] |= 1;
    Program program;
    program.instructions = {decode(words)};
    CHECK(run(program, inputs).outputs[0] == v(-4, -3, -2, -1));
}

TEST_CASE("write masks and saturation limit what changes") {
    std::array<Vec4, k_input_count> inputs{};
    inputs[0] = v(-1, 0.5f, 2, 7);
    const Result result = run_program({{.vector_op = 0x01,
                                        .sources = {source(SourceType::input), 0, 0},
                                        .vector_to_output = true,
                                        .vector_mask = 0b1010, // x and z
                                        .saturate = true}},
                                      inputs);
    CHECK(result.outputs[0] == v(0, 0, 1, 0));
}

TEST_CASE("scalar operations read source 2's x and broadcast") {
    std::array<Vec4, k_input_count> inputs{};
    inputs[0] = v(4, 9, 16, 25);
    const Result result = run_program({{.scalar_op = 0x02, // RCP
                                        .sources = {0, 0, source(SourceType::input)},
                                        .scalar_to_output = true,
                                        .scalar_mask = 0b0100, // y
                                        .output = 1}},
                                      inputs);
    CHECK(result.outputs[1] == v(0, 0.25f, 0, 0));
}

TEST_CASE("an unsupported opcode is reported") {
    const Result result =
        run_program({{.vector_op = 0x0d, .sources = {source(SourceType::input), 0, 0}}}, {});
    REQUIRE(result.error);
    CHECK(*result.error == "vertex program vector opcode 0xd is not supported yet");
}
