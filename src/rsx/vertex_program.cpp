#include "rsx/vertex_program.h"

#include "rsx/shader_ops.h"

#include <format>

namespace rsx::vertex {

namespace {

using namespace shader_float;
using namespace shader_ops;

u32 bits(u32 word, u32 first, u32 count) {
    return (word >> first) & ((1u << count) - 1);
}

constexpr u32 k_no_vector_temp = 0x3f;
constexpr u32 k_no_scalar_temp = 0x1f;
constexpr u32 k_condition_true = 7;

// A source operand: register type in bits 0-1, temp in 2-6, swizzle in 8-15 (x highest), negate
// in 16.
Source decode_source(u32 field, bool absolute) {
    Source source;
    source.type = static_cast<SourceType>(bits(field, 0, 2));
    source.temp = bits(field, 2, 5);
    source.swizzle = {static_cast<u8>(bits(field, 14, 2)), static_cast<u8>(bits(field, 12, 2)),
                      static_cast<u8>(bits(field, 10, 2)), static_cast<u8>(bits(field, 8, 2))};
    source.negate = bits(field, 16, 1) != 0;
    source.absolute = absolute;
    return source;
}

// Write masks list x in their highest bit.
u32 decode_mask(u32 field) {
    return ((field >> 3) & 1) | ((field >> 1) & 2) | ((field << 1) & 4) | ((field << 3) & 8);
}

// Vector opcodes. nouveau: NVFX_VP_INST_VEC_OP_*.
enum VectorOp : u32 {
    vec_nop = 0x00,
    vec_mov = 0x01,
    vec_mul = 0x02,
    vec_add = 0x03,
    vec_mad = 0x04,
    vec_dp3 = 0x05,
    vec_dph = 0x06,
    vec_dp4 = 0x07,
    vec_dst = 0x08,
    vec_min = 0x09,
    vec_max = 0x0a,
    vec_slt = 0x0b,
    vec_sge = 0x0c,
    vec_frc = 0x0e,
    vec_flr = 0x0f,
    vec_seq = 0x10,
    vec_sfl = 0x11,
    vec_sgt = 0x12,
    vec_sle = 0x13,
    vec_sne = 0x14,
    vec_str = 0x15,
};

// Scalar opcodes. nouveau: NVFX_VP_INST_SCA_OP_*.
enum ScalarOp : u32 {
    sca_nop = 0x00,
    sca_mov = 0x01,
    sca_rcp = 0x02,
    sca_rcc = 0x03,
    sca_rsq = 0x04,
};

} // namespace

Instruction decode(std::span<const u32, 4> words) {
    Instruction instruction;
    const u32 w0 = words[0];
    const u32 w1 = words[1];
    const u32 w2 = words[2];
    const u32 w3 = words[3];

    instruction.vector_opcode = bits(w1, 22, 5);
    instruction.scalar_opcode = bits(w1, 27, 5);
    instruction.input = bits(w1, 8, 4);
    // nouveau masks 8 bits here; envytools gives the field 10, enough for every constant.
    instruction.constant = bits(w1, 12, 10);

    const u32 source0 = (bits(w1, 0, 8) << 9) | bits(w2, 23, 9);
    const u32 source1 = bits(w2, 6, 17);
    const u32 source2 = (bits(w2, 0, 6) << 11) | bits(w3, 21, 11);
    instruction.sources = {decode_source(source0, bits(w0, 21, 1) != 0),
                           decode_source(source1, bits(w0, 22, 1) != 0),
                           decode_source(source2, bits(w0, 23, 1) != 0)};
    instruction.saturate = bits(w0, 26, 1) != 0;

    if (const u32 temp = bits(w0, 15, 6); temp != k_no_vector_temp) instruction.vector_temp = temp;
    instruction.vector_to_output = bits(w0, 30, 1) != 0;
    instruction.vector_mask = decode_mask(bits(w3, 13, 4));
    if (const u32 temp = bits(w3, 7, 5); temp != k_no_scalar_temp) instruction.scalar_temp = temp;
    instruction.scalar_to_output = bits(w3, 12, 1) != 0;
    instruction.scalar_mask = decode_mask(bits(w3, 17, 4));
    instruction.output = bits(w3, 2, 5);

    instruction.condition_test = bits(w0, 13, 1) != 0 && bits(w0, 10, 3) != k_condition_true;
    instruction.last = bits(w3, 0, 1) != 0;
    return instruction;
}

std::optional<std::vector<Instruction>> decode_program(std::span<const u32> memory, u32 start) {
    std::vector<Instruction> program;
    for (u32 slot = start; slot < memory.size() / 4; ++slot) {
        program.push_back(decode(memory.subspan(4 * slot).first<4>()));
        if (program.back().last) return program;
    }
    return std::nullopt;
}

namespace {

struct Machine {
    const Program& program;
    const std::array<Vec4, k_input_count>& inputs;
    std::array<Vec4, k_temp_count> temps{};
    Result result;

    Vec4 read(const Instruction& instruction, const Source& source) const {
        Vec4 value{};
        switch (source.type) {
        case SourceType::temp: value = temps[source.temp]; break;
        case SourceType::input: value = inputs[instruction.input]; break;
        case SourceType::constant: value = program.constants[instruction.constant]; break;
        case SourceType::unused: break;
        }
        return modify(value, source.swizzle, source.absolute, source.negate);
    }

    void write(Vec4& destination, const Vec4& value, u32 mask, bool saturated) {
        for (int i = 0; i < 4; ++i) {
            if (mask & (1u << i)) destination[i] = saturated ? saturate(value[i]) : value[i];
        }
    }

    std::optional<Vec4> vector_op(const Instruction& in, const Vec4& a, const Vec4& b,
                                  const Vec4& c) {
        switch (in.vector_opcode) {
        case vec_mov: return a;
        case vec_mul: return each(a, b, multiply);
        // UNVERIFIED: ADD takes its second operand from source 2, as if the vector unit were
        // built around MAD (a * b + c). envytools doesn't document which sources each opcode
        // reads, only that scalar operations read source 2.
        case vec_add: return each(a, c, add);
        case vec_mad:
            return Vec4{multiply_add(a[0], b[0], c[0]), multiply_add(a[1], b[1], c[1]),
                        multiply_add(a[2], b[2], c[2]), multiply_add(a[3], b[3], c[3])};
        case vec_dp3: return splat(dot(a, b, 3));
        case vec_dp4: return splat(dot(a, b, 4));
        case vec_dph: return splat(add(dot(a, b, 3), b[3]));
        case vec_dst: return Vec4{k_one, multiply(a[1], b[1]), a[2], b[3]};
        case vec_min: return each(a, b, minimum);
        case vec_max: return each(a, b, maximum);
        case vec_slt: return set_if(a, b, [](Float x, Float y) { return less(x, y); });
        case vec_sge: return set_if(a, b, greater_or_equal);
        case vec_sgt: return set_if(a, b, [](Float x, Float y) { return less(y, x); });
        case vec_sle: return set_if(a, b, [](Float x, Float y) { return greater_or_equal(y, x); });
        case vec_seq: return set_if(a, b, equal);
        case vec_sne: return set_if(a, b, [](Float x, Float y) { return !equal(x, y); });
        case vec_sfl: return splat(k_zero);
        case vec_str: return splat(k_one);
        case vec_frc: return Vec4{fraction(a[0]), fraction(a[1]), fraction(a[2]), fraction(a[3])};
        case vec_flr: return Vec4{floor(a[0]), floor(a[1]), floor(a[2]), floor(a[3])};
        default:
            result.error = std::format("vertex program vector opcode {:#x} is not supported yet",
                                       in.vector_opcode);
            return std::nullopt;
        }
    }

    // Scalar operations read source 2's (swizzled) x component.
    std::optional<Float> scalar_op(const Instruction& in, const Vec4& c) {
        switch (in.scalar_opcode) {
        case sca_mov: return c[0];
        case sca_rcp: return reciprocal(c[0]);
        case sca_rsq: return reciprocal_square_root(c[0] & ~k_sign); // RSQ uses |x|
        default:
            result.error = std::format("vertex program scalar opcode {:#x} is not supported yet",
                                       in.scalar_opcode);
            return std::nullopt;
        }
    }

    bool step(const Instruction& in) {
        if (in.condition_test) {
            result.error = "conditional vertex program instructions are not supported yet";
            return false;
        }
        const Vec4 a = read(in, in.sources[0]);
        const Vec4 b = read(in, in.sources[1]);
        const Vec4 c = read(in, in.sources[2]);

        std::optional<Vec4> vector;
        std::optional<Float> scalar;
        if (in.vector_opcode != vec_nop && !(vector = vector_op(in, a, b, c))) return false;
        if (in.scalar_opcode != sca_nop && !(scalar = scalar_op(in, c))) return false;

        if (vector) {
            if (in.vector_temp) write(temps[*in.vector_temp], *vector, in.vector_mask, in.saturate);
            if (in.vector_to_output) {
                write(result.outputs[in.output], *vector, in.vector_mask, in.saturate);
            }
        }
        if (scalar) {
            const Vec4 value = splat(*scalar);
            if (in.scalar_temp) write(temps[*in.scalar_temp], value, in.scalar_mask, in.saturate);
            if (in.scalar_to_output) {
                write(result.outputs[in.output], value, in.scalar_mask, in.saturate);
            }
        }
        return true;
    }
};

} // namespace

Result run(const Program& program, const std::array<Vec4, k_input_count>& inputs) {
    Machine machine{program, inputs, {}, {}};
    for (const Instruction& instruction : program.instructions) {
        if (!machine.step(instruction)) break;
    }
    return machine.result;
}

} // namespace rsx::vertex
