#pragma once

// RSX fragment programs: decoding them from memory, and running them on one fragment.
//
// Instructions are four 32-bit words, each stored with its 16-bit halves swapped. A source that
// reads a constant takes it from the four words right after the instruction. The program ends at
// the instruction with its "end" bit set, and its result is the color in R0. Field layout: Mesa's
// nouveau driver (nv30/nvfx_shader.h).

#include "common/types.h"
#include "core/memory.h"
#include "rsx/shader_float.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace rsx {

using shader_float::Vec4;

namespace fragment {

constexpr u32 k_temp_count = 64;
constexpr u32 k_max_instructions = 4096; // UNVERIFIED: the RSX's limit

// Input registers. nouveau: NVFX_FP_OP_INPUT_SRC_*.
constexpr u32 k_input_position = 0;
constexpr u32 k_input_color0 = 1;
constexpr u32 k_input_color1 = 2;
constexpr u32 k_input_fog = 3;
constexpr u32 k_input_texcoord0 = 4; // through 13
constexpr u32 k_input_count = 15;

enum class SourceType : u32 { temp = 0, input = 1, constant = 2 };

struct Source {
    SourceType type = SourceType::temp;
    u32 temp = 0;
    bool half = false; // an fp16 register, which we don't support yet
    std::array<u8, 4> swizzle = {0, 1, 2, 3};
    bool negate = false;
    bool absolute = false;
};

struct Instruction {
    u32 opcode = 0;
    u32 destination = 0;
    bool destination_half = false;
    bool no_destination = false;
    u32 mask = 0; // bit 0 = x ... bit 3 = w
    u32 input = 0;
    bool saturate = false;
    u32 scale = 0;            // output scale: 0 means none
    bool conditional = false; // executes only on some condition codes: not supported yet
    bool is_branch = false;   // a flow-control instruction: not supported yet
    std::array<Source, 3> sources;
    Vec4 constant{}; // the inline constant, if any source reads one
    bool end = false;
};

using Program = std::vector<Instruction>;

// Decodes the program at `address`. Fails if no instruction ends it within the limit.
std::optional<Program> decode_program(const Memory& memory, u32 address);

struct Result {
    Vec4 color{};
    std::optional<std::string> error; // an operation we don't support yet
};

Result run(const Program& program, const std::array<Vec4, k_input_count>& inputs);

} // namespace fragment

} // namespace rsx
