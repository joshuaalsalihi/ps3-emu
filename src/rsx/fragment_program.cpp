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
    op_dst = 0x07,
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
    op_kil = 0x12,
    op_ddx = 0x15,
    op_ddy = 0x16,
    op_tex = 0x17,
    op_txp = 0x18,
    op_rcp = 0x1a,
    op_rsq = 0x1b,
    op_ex2 = 0x1c,
    op_lg2 = 0x1d,
    op_str = 0x20,
    op_sfl = 0x21,
    op_txl = 0x2f,
    op_txb = 0x31,
    op_dp2 = 0x38,
    op_nrm = 0x39,
    op_div = 0x3a,
};

} // namespace

std::optional<Program> decode_program(const Memory& memory, u32 address) {
    Program program;
    while (program.instructions.size() < k_max_instructions) {
        const u32 w0 = read_word(memory, address);
        const u32 w1 = read_word(memory, address + 4);
        const u32 w2 = read_word(memory, address + 8);
        const u32 w3 = read_word(memory, address + 12);
        address += 16;

        Instruction instruction;
        instruction.end = bits(w0, 0, 1) != 0;
        instruction.destination = bits(w0, 1, 6);
        instruction.destination_half = bits(w0, 7, 1) != 0;
        instruction.set_condition = bits(w0, 8, 1) != 0;
        instruction.mask = bits(w0, 9, 4);
        instruction.input = bits(w0, 13, 4);
        instruction.texture_unit = bits(w0, 17, 4);
        instruction.precision = static_cast<Precision>(bits(w0, 22, 2));
        instruction.opcode = bits(w0, 24, 6);
        instruction.no_destination = bits(w0, 30, 1) != 0;
        instruction.saturate = bits(w0, 31, 1) != 0;
        instruction.condition = bits(w1, 18, 3);
        instruction.condition_swizzle = {
            static_cast<u8>(bits(w1, 21, 2)), static_cast<u8>(bits(w1, 23, 2)),
            static_cast<u8>(bits(w1, 25, 2)), static_cast<u8>(bits(w1, 27, 2))};
        instruction.scale = bits(w2, 28, 3);
        instruction.is_branch = bits(w2, 31, 1) != 0;
        // UNVERIFIED: w3 bit 31 disables perspective correction for an input. cgcomp sets it
        // only where a scale doesn't change the result (such as NRM of a texture coordinate),
        // so we read every input corrected.
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

        program.instructions.push_back(instruction);
        if (instruction.end) return program;
    }
    return std::nullopt;
}

bool is_texture_lookup(const Instruction& instruction) {
    switch (instruction.opcode) {
    case op_tex:
    case op_txp:
    case op_txb:
    case op_txl: return true;
    default: return false;
    }
}

namespace {

// Neighbors in a quad: lane ^ 1 is beside a lane, lane ^ 2 above or below it.
constexpr u32 k_horizontal = 1;
constexpr u32 k_vertical = 2;

// A condition code component: how the value that set it compares with zero. NaN is unordered,
// which only "not equal" (and "always") accept, as with NV_fragment_program's UN.
constexpr u8 k_unordered = 0;

u8 classify(Float value) {
    if ((value & 0x7fff'ffff) > 0x7f80'0000) return k_unordered;
    if (less(value, k_zero)) return k_condition_less;
    if (less(k_zero, value)) return k_condition_greater;
    return k_condition_equal;
}

bool passes(u32 condition, u8 code) {
    if (code == k_unordered)
        return condition == k_condition_less + k_condition_greater ||
               condition == k_condition_always;
    return (condition & code) != 0;
}

Float with_precision(Precision precision, Float value) {
    switch (precision) {
    case Precision::fp32: return value;
    case Precision::fp16: return round_to_half(value);
    case Precision::fx12: return round_to_fixed12(value);
    }
    return value;
}

struct Lane {
    std::array<Vec4, k_temp_count> temps{};
    std::array<Vec4, k_temp_count> halves{};
    // UNVERIFIED: the condition code starts as "equal", as if set from 0.
    std::array<u8, 4> condition{k_condition_equal, k_condition_equal, k_condition_equal,
                                k_condition_equal};
};

struct Machine {
    const Quad<Inputs>& inputs;
    const Sampler* sampler;
    Quad<Lane> lanes{};
    QuadResult result;

    Vec4 read(u32 lane, const Instruction& in, const Source& source) const {
        Vec4 value{};
        switch (source.type) {
        case SourceType::temp:
            value = source.half ? lanes[lane].halves[source.temp] : lanes[lane].temps[source.temp];
            break;
        case SourceType::input: value = inputs[lane][in.input]; break;
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
        case op_dp2: return splat(dot(a, b, 2));
        case op_dp3: return splat(dot(a, b, 3));
        case op_dp4: return splat(dot(a, b, 4));
        case op_dst: return Vec4{k_one, multiply(a[1], b[1]), a[2], b[3]};
        case op_min: return each(a, b, minimum);
        case op_max: return each(a, b, maximum);
        case op_slt: return set_if(a, b, [](Float x, Float y) { return less(x, y); });
        case op_sge: return set_if(a, b, greater_or_equal);
        case op_sle: return set_if(a, b, [](Float x, Float y) { return greater_or_equal(y, x); });
        case op_sgt: return set_if(a, b, [](Float x, Float y) { return less(y, x); });
        case op_sne: return set_if(a, b, [](Float x, Float y) { return !equal(x, y); });
        case op_seq: return set_if(a, b, equal);
        case op_str: return splat(k_one);
        case op_sfl: return splat(k_zero);
        case op_frc: return Vec4{fraction(a[0]), fraction(a[1]), fraction(a[2]), fraction(a[3])};
        case op_flr: return Vec4{floor(a[0]), floor(a[1]), floor(a[2]), floor(a[3])};
        case op_rcp: return splat(reciprocal(a[0]));
        // UNVERIFIED: RSQ uses |x|, as ARB_fragment_program requires; cgcomp sets no
        // absolute-value bit for it, so the hardware must take it.
        case op_rsq: return splat(reciprocal_square_root(a[0] & ~k_sign));
        case op_ex2: return splat(exp2(a[0]));
        case op_lg2: return splat(log2(a[0]));
        // UNVERIFIED: NRM scales w along with x, y and z (programs mask it off).
        case op_nrm: return each(a, splat(reciprocal_square_root(dot(a, a, 3))), multiply);
        case op_div: return each(a, splat(b[0]), divide);
        default:
            result.error =
                std::format("fragment program opcode {:#x} is not supported yet", in.opcode);
            return std::nullopt;
        }
    }

    bool supported(const Instruction& in) {
        if (in.is_branch) result.error = "fragment program flow control is not supported yet";
        if (in.scale != 0) result.error = "fragment program output scaling is not supported yet";
        if (in.precision != Precision::fp32 && in.precision != Precision::fp16 &&
            in.precision != Precision::fx12) {
            result.error = "fragment program precision 3 is invalid";
        }
        if (is_texture_lookup(in) && !sampler) result.error = "texture lookups need a sampler";
        return !result.error;
    }

    // The value of each lane: from its own sources, or (for derivatives and lookups) from the
    // whole quad's.
    std::optional<Quad<Vec4>> values(const Instruction& in) {
        Quad<Vec4> a;
        for (u32 lane = 0; lane < k_quad_size; ++lane) {
            a[lane] = read(lane, in, in.sources[0]);
        }
        switch (in.opcode) {
        case op_ddx: return difference(a, k_horizontal);
        case op_ddy: return difference(a, k_vertical);
        case op_tex: return sampler->sample(in.texture_unit, a, Lookup::plain);
        case op_txp: return sampler->sample(in.texture_unit, projected(a), Lookup::plain);
        case op_txb: return sampler->sample(in.texture_unit, a, Lookup::bias);
        case op_txl: return sampler->sample(in.texture_unit, a, Lookup::level);
        default: break;
        }
        Quad<Vec4> value;
        for (u32 lane = 0; lane < k_quad_size; ++lane) {
            const auto lane_value =
                compute(in, a[lane], read(lane, in, in.sources[1]), read(lane, in, in.sources[2]));
            if (!lane_value) return std::nullopt;
            value[lane] = *lane_value;
        }
        return value;
    }

    // Each lane's neighbor across `axis` minus the lane on the quad's top or left side.
    // UNVERIFIED: both lanes of a pair get the same difference, and y grows downward (the
    // window origin libgcm sets is the top left).
    static Quad<Vec4> difference(const Quad<Vec4>& a, u32 axis) {
        Quad<Vec4> result;
        for (u32 lane = 0; lane < k_quad_size; ++lane) {
            const u32 first = lane & ~axis;
            result[lane] = each(a[first | axis], a[first],
                                [](Float x, Float y) { return add(x, y ^ k_sign); });
        }
        return result;
    }

    // TXP: the coordinates divided by w.
    static Quad<Vec4> projected(const Quad<Vec4>& a) {
        Quad<Vec4> result;
        for (u32 lane = 0; lane < k_quad_size; ++lane) {
            result[lane] = each(a[lane], splat(a[lane][3]), divide);
        }
        return result;
    }

    bool component_passes(const Lane& lane, const Instruction& in, int component) const {
        return passes(in.condition, lane.condition[in.condition_swizzle[component]]);
    }

    // KIL discards the lanes where its condition passes in any component.
    void kill(const Instruction& in) {
        for (u32 lane = 0; lane < k_quad_size; ++lane) {
            for (int i = 0; i < 4; ++i) {
                if (component_passes(lanes[lane], in, i)) result.killed |= 1u << lane;
            }
        }
    }

    bool step(const Instruction& in) {
        if (in.opcode == op_nop) return true;
        if (!supported(in)) return false;
        if (in.opcode == op_kil) {
            kill(in);
            return true;
        }
        const auto value = values(in);
        if (!value) return false;
        for (u32 lane = 0; lane < k_quad_size; ++lane) {
            Lane& state = lanes[lane];
            Vec4& destination =
                in.destination_half ? state.halves[in.destination] : state.temps[in.destination];
            for (int i = 0; i < 4; ++i) {
                if ((in.mask & (1u << i)) == 0 || !component_passes(state, in, i)) continue;
                Float component = with_precision(in.precision, (*value)[lane][i]);
                if (in.saturate) component = saturate(component);
                if (in.set_condition) state.condition[i] = classify(component);
                if (in.no_destination) continue;
                destination[i] = in.destination_half ? round_to_half(component) : component;
            }
        }
        return true;
    }
};

} // namespace

QuadResult run(const Program& program, const Quad<Inputs>& inputs, const Sampler* sampler) {
    Machine machine{inputs, sampler, {}, {}};
    for (const Instruction& instruction : program.instructions) {
        if (!machine.step(instruction)) return machine.result;
    }
    for (u32 lane = 0; lane < k_quad_size; ++lane) {
        const Lane& state = machine.lanes[lane];
        machine.result.colors[lane] = program.half_output ? state.halves[0] : state.temps[0];
    }
    return machine.result;
}

Result run(const Program& program, const Inputs& inputs, const Sampler* sampler) {
    const QuadResult quad = run(program, Quad<Inputs>{inputs, inputs, inputs, inputs}, sampler);
    return {quad.colors[0], (quad.killed & 1) != 0, quad.error};
}

} // namespace rsx::fragment
