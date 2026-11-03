#pragma once

// RSX fragment programs: decoding them from memory, and running them on a quad of fragments.
//
// Instructions are four 32-bit words, each stored with its 16-bit halves swapped. A source that
// reads a constant takes it from the four words right after the instruction. The program ends at
// the instruction with its "end" bit set, and its result is the color in R0. Field layout: Mesa's
// nouveau driver (nv30/nvfx_shader.h).
//
// Fragments are shaded in 2x2 quads, as on the GPUs of the RSX's generation, so that a program
// can tell how fast a value changes across the screen: DDX and DDY, and the level of detail of a
// texture lookup, compare neighboring lanes.
//
// Registers: 64 full-precision temps (R) and 64 half-precision ones (H), and a condition code
// with four components. Each instruction computes at a precision (fp32, fp16 or 12-bit fixed
// point), may set the condition code from its result, and may write only the components whose
// condition code passes a test. UNVERIFIED: on the hardware, H registers share storage with R
// registers (H2n and H2n+1 are halves of Rn); cgcomp never uses both views of one register, so
// ours are apart.

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
    bool half = false; // an H register
    std::array<u8, 4> swizzle = {0, 1, 2, 3};
    bool negate = false;
    bool absolute = false;
};

enum class Precision : u32 { fp32 = 0, fp16 = 1, fx12 = 2 };

// Condition tests: which of the condition code's values (less than, equal to or greater than
// zero) let a component execute. nouveau: NVFX_FP_OP_COND_*.
constexpr u32 k_condition_less = 1;
constexpr u32 k_condition_equal = 2;
constexpr u32 k_condition_greater = 4;
constexpr u32 k_condition_always = 7;

struct Instruction {
    u32 opcode = 0;
    u32 destination = 0;
    bool destination_half = false; // an H register
    bool no_destination = false;
    u32 mask = 0; // bit 0 = x ... bit 3 = w
    u32 input = 0;
    bool saturate = false;
    Precision precision = Precision::fp32;
    u32 scale = 0;              // output scale: 0 means none
    bool set_condition = false; // the result also sets the condition code
    u32 condition = k_condition_always;
    std::array<u8, 4> condition_swizzle = {0, 1, 2, 3}; // which condition component each tests
    bool is_branch = false; // a flow-control instruction: not supported yet
    u32 texture_unit = 0;   // the texture a lookup samples
    std::array<Source, 3> sources;
    Vec4 constant{}; // the inline constant, if any source reads one
    bool end = false;
};

struct Program {
    std::vector<Instruction> instructions;
    // The result is H0 rather than R0: bit 6 (32-bit exports) of the program's control word,
    // method::k_shader_control, is clear. cgcomp sets the bit when a program writes R0.
    bool half_output = false;
};

// Decodes the program at `address`. Fails if no instruction ends it within the limit.
std::optional<Program> decode_program(const Memory& memory, u32 address);

// Whether the instruction samples a texture (TEX, TXP, TXB, TXL).
bool is_texture_lookup(const Instruction& instruction);

// A quad's four lanes are the fragments at (x, y), (x + 1, y), (x, y + 1) and (x + 1, y + 1).
constexpr u32 k_quad_size = 4;
template <typename T>
using Quad = std::array<T, k_quad_size>;
using Inputs = std::array<Vec4, k_input_count>;

// How a lookup chooses its level of detail: from the quad's coordinates (TEX, TXP), from them plus
// a bias in w (TXB), or as given in w (TXL).
enum class Lookup { plain, bias, level };

// Samples textures; a backend provides it.
class Sampler {
public:
    virtual ~Sampler() = default;
    virtual Quad<Vec4> sample(u32 unit, const Quad<Vec4>& coordinates, Lookup lookup) const = 0;
};

struct QuadResult {
    Quad<Vec4> colors{};
    u32 killed = 0;                   // bit i: KIL discarded lane i
    std::optional<std::string> error; // an operation we don't support yet
};

QuadResult run(const Program& program, const Quad<Inputs>& inputs, const Sampler* sampler);

struct Result {
    Vec4 color{};
    bool killed = false;
    std::optional<std::string> error;
};

// Runs one fragment, as a quad whose lanes all hold it (so every derivative is 0).
Result run(const Program& program, const Inputs& inputs, const Sampler* sampler = nullptr);

} // namespace fragment

} // namespace rsx
