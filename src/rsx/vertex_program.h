#pragma once

// RSX vertex programs: decoding the 128-bit instructions, and running them on one vertex.
//
// Each instruction has a vector operation and a scalar operation, which run side by side: both
// read their sources first, then write their results. Field layout: Mesa's nouveau driver
// (nv30/nv40_vertprog.h, nvfx_shader.h); the instruction table bit ranges agree with envytools'
// "Curie" XF ISA. The program ends at the instruction with its "last" bit set.

#include "common/types.h"
#include "rsx/shader_float.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rsx {

using shader_float::Vec4;

namespace vertex {

constexpr u32 k_input_count = 16;
constexpr u32 k_output_count = 17; // position, colors, fog, point size, texture coordinates 0-9
constexpr u32 k_temp_count = 32;
constexpr u32 k_constant_count = 512; // UNVERIFIED: the RSX's constant memory size
constexpr u32 k_instruction_slots = 512;

// Output registers. nouveau: NV40_VP_INST_DEST_*.
constexpr u32 k_output_position = 0;
constexpr u32 k_output_color0 = 1;
constexpr u32 k_output_color1 = 2;
constexpr u32 k_output_fog = 5;
constexpr u32 k_output_texcoord0 = 7;

enum class SourceType : u32 { unused = 0, temp = 1, input = 2, constant = 3 };

struct Source {
    SourceType type = SourceType::unused;
    u32 temp = 0;
    std::array<u8, 4> swizzle = {0, 1, 2, 3}; // which component each of x, y, z, w reads
    bool negate = false;
    bool absolute = false;
};

struct Instruction {
    u32 vector_opcode = 0;
    u32 scalar_opcode = 0;
    u32 input = 0;    // the input register any input source reads
    u32 constant = 0; // the constant register any constant source reads
    std::array<Source, 3> sources;
    bool saturate = false;

    std::optional<u32> vector_temp; // destinations: a temp and/or the output register
    bool vector_to_output = false;
    u32 vector_mask = 0; // bit 0 = x ... bit 3 = w
    std::optional<u32> scalar_temp;
    bool scalar_to_output = false;
    u32 scalar_mask = 0;
    u32 output = 0;

    bool condition_test = false; // conditional execution, which we don't support yet
    bool last = false;
};

Instruction decode(std::span<const u32, 4> words);

// A program ready to run: its instructions from the start slot to the last one.
struct Program {
    std::vector<Instruction> instructions;
    std::array<Vec4, k_constant_count> constants{};
};

// Decodes the program starting at `start` in the RSX's program memory (4 words per slot).
// Fails if no instruction ends it.
std::optional<std::vector<Instruction>> decode_program(std::span<const u32> memory, u32 start);

struct Result {
    std::array<Vec4, k_output_count> outputs{};
    std::optional<std::string> error; // an operation we don't support yet
};

// Runs `program` on one vertex's inputs.
Result run(const Program& program, const std::array<Vec4, k_input_count>& inputs);

} // namespace vertex

} // namespace rsx
