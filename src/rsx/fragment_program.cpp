#include "rsx/fragment_program.h"

#include "rsx/shader_ops.h"

#include <algorithm>
#include <format>

namespace rsx::fragment {

namespace {

using namespace shader_float;
using namespace shader_ops;

u32 bits(u32 word, u32 first, u32 count) {
    return (word >> first) & ((1u << count) - 1);
}

// Each word is stored with its halves swapped.
u32 read_word(const Memory& memory, u32 address) {
    const u32 stored = memory.read<u32>(address);
    return (stored << 16) | (stored >> 16);
}

constexpr u32 k_always = 7; // execute on less, equal and greater: unconditionally

// Common source fields: type in bits 0-1, register in 2-7, fp16 in 8, swizzle in 9-16 (x
// lowest), negate in 17.
Source decode_source(u32 word, bool absolute) {
    Source source;
    source.type = static_cast<SourceType>(bits(word, 0, 2));
    source.temp = bits(word, 2, 6);
    source.half = bits(word, 8, 1) != 0;
    source.swizzle = {static_cast<u8>(bits(word, 9, 2)), static_cast<u8>(bits(word, 11, 2)),
                      static_cast<u8>(bits(word, 13, 2)), static_cast<u8>(bits(word, 15, 2))};
    source.negate = bits(word, 17, 1) != 0;
    source.absolute = absolute;
    return source;
}

// Opcodes. nouveau: NVFX_FP_OP_OPCODE_*.
enum Op : u32 {
    op_nop = 0x00,
    op_mov = 0x01,
    op_mul = 0x02,
    op_add = 0x03,
    op_mad = 0x04,
    op_dp3 = 0x05,
    op_dp4 = 0x06,
    op_min = 0x08,
    op_max = 0x09,
    op_slt = 0x0a,
    op_sge = 0x0b,
    op_sle = 0x0c,
    op_sgt = 0x0d,
    op_sne = 0x0e,
    op_seq = 0x0f,
    op_frc = 0x10,
    op_flr = 0x11,
    op_rcp = 0x1a,
    op_div = 0x3a,
};

} // namespace

std::optional<Program> decode_program(const Memory& memory, u32 address) {
    Program program;
    while (program.size() < k_max_instructions) {
        const u32 w0 = read_word(memory, address);
        const u32 w1 = read_word(memory, address + 4);
        const u32 w2 = read_word(memory, address + 8);
        const u32 w3 = read_word(memory, address + 12);
        address += 16;

        Instruction instruction;
        instruction.end = bits(w0, 0, 1) != 0;
        instruction.destination = bits(w0, 1, 6);
        instruction.destination_half = bits(w0, 7, 1) != 0;
        instruction.mask = bits(w0, 9, 4);
        instruction.input = bits(w0, 13, 4);
        instruction.opcode = bits(w0, 24, 6);
        instruction.no_destination = bits(w0, 30, 1) != 0;
        instruction.saturate = bits(w0, 31, 1) != 0;
        instruction.conditional = bits(w1, 18, 3) != k_always;
        instruction.scale = bits(w2, 28, 3);
        instruction.is_branch = bits(w2, 31, 1) != 0;
        instruction.sources = {decode_source(w1, bits(w1, 29, 1) != 0),
                               decode_source(w2, bits(w2, 18, 1) != 0),
                               decode_source(w3, bits(w3, 18, 1) != 0)};

        const bool has_constant =
            std::any_of(instruction.sources.begin(), instruction.sources.end(),
                        [](const Source& source) { return source.type == SourceType::constant; });
        if (has_constant) {
            for (u32 i = 0; i < 4; ++i) {
                instruction.constant[i] = read_word(memory, address + 4 * i);
            }
            address += 16;
        }

        program.push_back(instruction);
        if (instruction.end) return program;
    }
    return std::nullopt;
}

namespace {

struct Machine {
    const std::array<Vec4, k_input_count>& inputs;
    std::array<Vec4, k_temp_count> temps{};
    Result result;

    Vec4 read(const Instruction& in, const Source& source) const {
        Vec4 value{};
        switch (source.type) {
        case SourceType::temp: value = temps[source.temp]; break;
        case SourceType::input: value = inputs[in.input]; break;
        case SourceType::constant: value = in.constant; break;
        }
        return modify(value, source.swizzle, source.absolute, source.negate);
    }

    std::optional<Vec4> compute(const Instruction& in, const Vec4& a, const Vec4& b,
                                const Vec4& c) {
        switch (in.opcode) {
        case op_mov: return a;
        case op_mul: return each(a, b, multiply);
        case op_add: return each(a, b, add);
        case op_mad:
            return Vec4{multiply_add(a[0], b[0], c[0]), multiply_add(a[1], b[1], c[1]),
                        multiply_add(a[2], b[2], c[2]), multiply_add(a[3], b[3], c[3])};
        case op_dp3: return splat(dot(a, b, 3));
        case op_dp4: return splat(dot(a, b, 4));
        case op_min: return each(a, b, minimum);
        case op_max: return each(a, b, maximum);
        case op_slt: return set_if(a, b, [](Float x, Float y) { return less(x, y); });
        case op_sge: return set_if(a, b, greater_or_equal);
        case op_sle: return set_if(a, b, [](Float x, Float y) { return greater_or_equal(y, x); });
        case op_sgt: return set_if(a, b, [](Float x, Float y) { return less(y, x); });
        case op_sne: return set_if(a, b, [](Float x, Float y) { return !equal(x, y); });
        case op_seq: return set_if(a, b, equal);
        case op_frc: return Vec4{fraction(a[0]), fraction(a[1]), fraction(a[2]), fraction(a[3])};
        case op_flr: return Vec4{floor(a[0]), floor(a[1]), floor(a[2]), floor(a[3])};
        case op_rcp: return splat(reciprocal(a[0]));
        case op_div: return each(a, splat(b[0]), divide);
        default:
            result.error =
                std::format("fragment program opcode {:#x} is not supported yet", in.opcode);
            return std::nullopt;
        }
    }

    bool supported(const Instruction& in) {
        const bool half =
            in.destination_half || std::any_of(in.sources.begin(), in.sources.end(),
                                               [](const Source& source) { return source.half; });
        if (half) result.error = "fp16 fragment program registers are not supported yet";
        if (in.conditional)
            result.error = "conditional fragment program instructions are not supported yet";
        if (in.is_branch) result.error = "fragment program flow control is not supported yet";
        if (in.scale != 0) result.error = "fragment program output scaling is not supported yet";
        return !result.error;
    }

    bool step(const Instruction& in) {
        if (in.opcode == op_nop) return true;
        if (!supported(in)) return false;
        const auto value =
            compute(in, read(in, in.sources[0]), read(in, in.sources[1]), read(in, in.sources[2]));
        if (!value) return false;
        if (in.no_destination) return true;
        for (int i = 0; i < 4; ++i) {
            if (in.mask & (1u << i)) {
                temps[in.destination][i] = in.saturate ? saturate((*value)[i]) : (*value)[i];
            }
        }
        return true;
    }
};

} // namespace

Result run(const Program& program, const std::array<Vec4, k_input_count>& inputs) {
    Machine machine{inputs, {}, {}};
    for (const Instruction& instruction : program) {
        if (!machine.step(instruction)) return machine.result;
    }
    machine.result.color = machine.temps[0];
    return machine.result;
}

} // namespace rsx::fragment
