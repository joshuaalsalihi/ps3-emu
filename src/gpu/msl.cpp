#include "gpu/msl.h"

#include "gpu/vertex_input.h"

#include <algorithm>
#include <format>

namespace gpu {

namespace {

constexpr char k_components[] = "xyzw";

std::string swizzle(const std::array<u8, 4>& selectors) {
    std::string text = ".";
    for (const u8 selector : selectors) {
        text += k_components[selector];
    }
    return text;
}

// A modified source: swizzle, then absolute value, then negation.
std::string modify(std::string value, const std::array<u8, 4>& selectors, bool absolute,
                   bool negate) {
    value += swizzle(selectors);
    if (absolute) value = "abs(" + value + ")";
    if (negate) value = "-" + value;
    return value;
}

// "dst.xz = clamp(value, 0.0, 1.0).xz;" for the masked components.
std::string masked_write(const std::string& destination, const std::string& value, u32 mask,
                         bool saturate) {
    std::string components;
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i)) components += k_components[i];
    }
    if (components.empty()) return "";
    const std::string result = saturate ? "clamp(" + value + ", 0.0, 1.0)" : "(" + value + ")";
    return std::format("    {}.{} = {}.{};\n", destination, components, result, components);
}

std::string float_bits(u32 bits) {
    return std::format("as_type<float>({:#010x}u)", bits);
}

// Comparisons give 1.0 where they hold.
std::string set_if(const char* comparison) {
    return std::format("float4(a {} b)", comparison);
}

// ---- Vertex programs ----

std::string vertex_source(const rsx::vertex::Instruction& in, const rsx::vertex::Source& source) {
    using rsx::vertex::SourceType;
    std::string value = "float4(0.0)";
    switch (source.type) {
    case SourceType::temp: value = std::format("r[{}]", source.temp); break;
    case SourceType::input: value = std::format("v[{}]", in.input); break;
    case SourceType::constant: value = std::format("c[{}]", in.constant); break;
    case SourceType::unused: break;
    }
    return modify(value, source.swizzle, source.absolute, source.negate);
}

std::optional<std::string> vector_expression(u32 opcode) {
    switch (opcode) {
    case 0x01: return "a";
    case 0x02: return "a * b";
    case 0x03: return "a + c"; // ADD reads sources 0 and 2, as in rsx::vertex::run
    case 0x04: return "a * b + c";
    case 0x05: return "float4(dot(a.xyz, b.xyz))";
    case 0x06: return "float4(dot(a.xyz, b.xyz) + b.w)";
    case 0x07: return "float4(dot(a, b))";
    case 0x08: return "float4(1.0, a.y * b.y, a.z, b.w)";
    case 0x09: return "min(a, b)";
    case 0x0a: return "max(a, b)";
    case 0x0b: return set_if("<");
    case 0x0c: return set_if(">=");
    case 0x0e: return "fract(a)";
    case 0x0f: return "floor(a)";
    case 0x10: return set_if("==");
    case 0x11: return "float4(0.0)";
    case 0x12: return set_if(">");
    case 0x13: return set_if("<=");
    case 0x14: return set_if("!=");
    case 0x15: return "float4(1.0)";
    default: return std::nullopt;
    }
}

std::optional<std::string> scalar_expression(u32 opcode) {
    switch (opcode) {
    case 0x01: return "float4(c.x)";
    case 0x02: return "float4(1.0 / c.x)";
    case 0x04: return "float4(rsqrt(abs(c.x)))";
    default: return std::nullopt;
    }
}

std::optional<std::string> translate_vertex(const std::vector<rsx::vertex::Instruction>& program,
                                            std::string& body) {
    for (const rsx::vertex::Instruction& in : program) {
        if (in.condition_test) return "conditional vertex program instructions";
        body += "    {\n";
        body += std::format("    float4 a = {};\n    float4 b = {};\n    float4 c = {};\n",
                            vertex_source(in, in.sources[0]), vertex_source(in, in.sources[1]),
                            vertex_source(in, in.sources[2]));
        if (in.vector_opcode != 0) {
            const auto expression = vector_expression(in.vector_opcode);
            if (!expression) return std::format("vertex vector opcode {:#x}", in.vector_opcode);
            body += std::format("    float4 vector = {};\n", *expression);
        }
        if (in.scalar_opcode != 0) {
            const auto expression = scalar_expression(in.scalar_opcode);
            if (!expression) return std::format("vertex scalar opcode {:#x}", in.scalar_opcode);
            body += std::format("    float4 scalar = {};\n", *expression);
        }
        // Both operations read their sources before either writes.
        const std::string output = std::format("o[{}]", in.output);
        if (in.vector_opcode != 0) {
            if (in.vector_temp) {
                body += masked_write(std::format("r[{}]", *in.vector_temp), "vector",
                                     in.vector_mask, in.saturate);
            }
            if (in.vector_to_output) {
                body += masked_write(output, "vector", in.vector_mask, in.saturate);
            }
        }
        if (in.scalar_opcode != 0) {
            if (in.scalar_temp) {
                body += masked_write(std::format("r[{}]", *in.scalar_temp), "scalar",
                                     in.scalar_mask, in.saturate);
            }
            if (in.scalar_to_output) {
                body += masked_write(output, "scalar", in.scalar_mask, in.saturate);
            }
        }
        body += "    }\n";
    }
    return std::nullopt;
}

// ---- Fragment programs ----

std::string fragment_source(const rsx::fragment::Instruction& in,
                            const rsx::fragment::Source& source) {
    using rsx::fragment::SourceType;
    std::string value;
    switch (source.type) {
    case SourceType::temp: value = std::format("r[{}]", source.temp); break;
    case SourceType::input: value = std::format("f[{}]", in.input); break;
    case SourceType::constant:
        value = std::format("float4({}, {}, {}, {})", float_bits(in.constant[0]),
                            float_bits(in.constant[1]), float_bits(in.constant[2]),
                            float_bits(in.constant[3]));
        break;
    }
    return modify(value, source.swizzle, source.absolute, source.negate);
}

std::optional<std::string> fragment_expression(u32 opcode) {
    switch (opcode) {
    case 0x01: return "a";
    case 0x02: return "a * b";
    case 0x03: return "a + b";
    case 0x04: return "a * b + c";
    case 0x05: return "float4(dot(a.xyz, b.xyz))";
    case 0x06: return "float4(dot(a, b))";
    case 0x08: return "min(a, b)";
    case 0x09: return "max(a, b)";
    case 0x0a: return set_if("<");
    case 0x0b: return set_if(">=");
    case 0x0c: return set_if("<=");
    case 0x0d: return set_if(">");
    case 0x0e: return set_if("!=");
    case 0x0f: return set_if("==");
    case 0x10: return "fract(a)";
    case 0x11: return "floor(a)";
    case 0x1a: return "float4(1.0 / a.x)";
    case 0x3a: return "a / b.x";
    default: return std::nullopt;
    }
}

std::optional<std::string> translate_fragment(const rsx::fragment::Program& program,
                                              std::string& body) {
    for (const rsx::fragment::Instruction& in : program) {
        if (in.opcode == 0) continue;
        const bool half =
            in.destination_half || std::any_of(in.sources.begin(), in.sources.end(),
                                               [](const auto& source) { return source.half; });
        if (half) return "fp16 fragment program registers";
        if (in.conditional || in.is_branch) return "fragment program flow control";
        if (in.scale != 0) return "fragment program output scaling";
        const auto expression = fragment_expression(in.opcode);
        if (!expression) return std::format("fragment opcode {:#x}", in.opcode);
        body += "    {\n";
        body += std::format("    float4 a = {};\n    float4 b = {};\n    float4 c = {};\n",
                            fragment_source(in, in.sources[0]), fragment_source(in, in.sources[1]),
                            fragment_source(in, in.sources[2]));
        if (!in.no_destination) {
            body += masked_write(std::format("r[{}]", in.destination), *expression, in.mask,
                                 in.saturate);
        }
        body += "    }\n";
    }
    return std::nullopt;
}

} // namespace

MslTranslation translate_to_msl(const std::vector<rsx::vertex::Instruction>& vertex_program,
                                const rsx::fragment::Program& fragment_program, bool flat_shading) {
    using namespace rsx;
    MslTranslation translation;
    std::string vertex_body;
    std::string fragment_body;
    if (auto missing = translate_vertex(vertex_program, vertex_body)) {
        translation.error = *missing + " can't be translated to Metal yet";
        return translation;
    }
    if (auto missing = translate_fragment(fragment_program, fragment_body)) {
        translation.error = *missing + " can't be translated to Metal yet";
        return translation;
    }

    const char* interpolation = flat_shading ? ", flat" : "";
    std::string varyings;
    for (u32 i = 1; i < vertex::k_output_count; ++i) {
        varyings += std::format("    float4 o{} [[user(o{}){}]];\n", i, i, interpolation);
    }
    std::string copy_outputs;
    for (u32 i = 1; i < vertex::k_output_count; ++i) {
        copy_outputs += std::format("    out.o{} = o[{}];\n", i, i);
    }
    std::string fragment_inputs =
        std::format("    f[{}] = float4(in.position.xy, 0.0, 1.0);\n", fragment::k_input_position);
    for (u32 input = 0; input < fragment::k_input_count; ++input) {
        if (const auto output = vertex_output_for(input)) {
            fragment_inputs += std::format("    f[{}] = in.o{};\n", input, *output);
        }
    }

    translation.source = std::format(
        R"(#include <metal_stdlib>
using namespace metal;
#pragma clang diagnostic ignored "-Wunused-variable" // not every instruction reads all sources

struct Varyings {{
    float4 position [[position]];
{}}};

struct Viewport {{
    float4 scale;
    float4 offset;
    float4 size; // the render target's width and height, in pixels
}};

vertex Varyings vertex_main(uint id [[vertex_id]], device const float4* inputs [[buffer(0)]],
                            constant float4* c [[buffer(1)]],
                            constant Viewport& viewport [[buffer(2)]]) {{
    float4 v[{}];
    for (int i = 0; i < {}; ++i) v[i] = inputs[id * {} + i];
    float4 r[{}] = {{}};
    float4 o[{}] = {{}};
{}
    // The RSX's viewport transform, then back to the clip space Metal maps onto the same pixels.
    float4 p = o[0];
    float3 window = p.xyz / p.w * viewport.scale.xyz + viewport.offset.xyz;
    Varyings out;
    out.position = float4((window.x / viewport.size.x * 2.0 - 1.0) * p.w,
                          (1.0 - window.y / viewport.size.y * 2.0) * p.w, window.z * p.w, p.w);
{}    return out;
}}

fragment float4 fragment_main(Varyings in [[stage_in]]) {{
    float4 f[{}] = {{}};
{}    float4 r[{}] = {{}};
{}    return r[0];
}}
)",
        varyings, vertex::k_input_count, vertex::k_input_count, vertex::k_input_count,
        vertex::k_temp_count, vertex::k_output_count, vertex_body, copy_outputs,
        fragment::k_input_count, fragment_inputs, fragment::k_temp_count, fragment_body);
    return translation;
}

} // namespace gpu
