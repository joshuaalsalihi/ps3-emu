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
    case SourceType::constant: value = std::format("constants[{}]", in.constant); break;
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
    case SourceType::temp:
        value = std::format("{}[{}]", source.half ? "h" : "r", source.temp);
        break;
    case SourceType::input: value = std::format("f[{}]", in.input); break;
    case SourceType::constant:
        value = std::format("float4({}, {}, {}, {})", float_bits(in.constant[0]),
                            float_bits(in.constant[1]), float_bits(in.constant[2]),
                            float_bits(in.constant[3]));
        break;
    }
    return modify(value, source.swizzle, source.absolute, source.negate);
}

// A texture lookup (TEX, TXP, TXB, TXL) with the texture's own LOD bias, in 1/256 levels, added.
std::optional<std::string> lookup_expression(const rsx::fragment::Instruction& in,
                                             const rsx::Texture& texture) {
    const std::string unit = std::format("t{0}.sample(s{0}, ", in.texture_unit);
    const std::string bias = std::format("{:#x}p-8", texture.lod_bias);
    const std::string texture_bias = texture.lod_bias == 0 ? "" : ", bias(" + bias + ")";
    switch (in.opcode) {
    case 0x17: return unit + "a.xy" + texture_bias + ")";
    case 0x18: return unit + "a.xy / a.w" + texture_bias + ")";
    case 0x31: return unit + "a.xy, bias(a.w + " + bias + "))";
    case 0x2f: return unit + "a.xy, level(a.w + " + bias + "))";
    default: return std::nullopt;
    }
}

std::optional<std::string> fragment_expression(u32 opcode) {
    switch (opcode) {
    case 0x01: return "a";
    case 0x02: return "a * b";
    case 0x03: return "a + b";
    case 0x04: return "a * b + c";
    case 0x05: return "float4(dot(a.xyz, b.xyz))";
    case 0x06: return "float4(dot(a, b))";
    case 0x07: return "float4(1.0, a.y * b.y, a.z, b.w)";
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
    case 0x15: return "dfdx(a)";
    case 0x16: return "dfdy(a)";
    case 0x1a: return "float4(1.0 / a.x)";
    case 0x1b: return "float4(rsqrt(abs(a.x)))";
    case 0x1c: return "float4(exp2(a.x))";
    case 0x1d: return "float4(log2(a.x))";
    case 0x20: return "float4(1.0)";
    case 0x21: return "float4(0.0)";
    case 0x38: return "float4(dot(a.xy, b.xy))";
    case 0x39: return "a * rsqrt(dot(a.xyz, a.xyz))";
    case 0x3a: return "a / b.x";
    default: return std::nullopt;
    }
}

using Textures = std::array<std::optional<rsx::Texture>, rsx::k_texture_units>;

// The condition test, per component, on the condition code: the values that last set it,
// compared with 0 (NaN fails every test but "not equal", as in rsx::fragment).
std::string condition_test(const rsx::fragment::Instruction& in) {
    static constexpr const char* k_tests[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (in.condition == 0 || in.condition == 7)
        return std::format("bool4({})", k_tests[in.condition]);
    return std::format("(cc{} {} 0.0)", swizzle(in.condition_swizzle), k_tests[in.condition]);
}

std::string with_precision(rsx::fragment::Precision precision, const std::string& value) {
    switch (precision) {
    case rsx::fragment::Precision::fp32: return value;
    case rsx::fragment::Precision::fp16: return "float4(half4(" + value + "))";
    case rsx::fragment::Precision::fx12:
        return "clamp(rint(" + value + " * 1024.0), -2048.0, 2047.0) / 1024.0";
    }
    return value;
}

std::optional<std::string> translate_fragment(const rsx::fragment::Program& program,
                                              const Textures& textures, std::string& body) {
    for (const rsx::fragment::Instruction& in : program.instructions) {
        if (in.opcode == 0) continue;
        if (in.is_branch) return "fragment program flow control";
        if (in.scale != 0) return "fragment program output scaling";
        body += "    {\n";
        body += std::format("    bool4 pass = {};\n", condition_test(in));
        if (in.opcode == 0x12) { // KIL
            body += "    if (any(pass)) discard_fragment();\n    }\n";
            continue;
        }
        const auto& texture = textures[in.texture_unit];
        if (rsx::fragment::is_texture_lookup(in) && !texture) return "a missing texture";
        const auto expression = rsx::fragment::is_texture_lookup(in)
                                    ? lookup_expression(in, *texture)
                                    : fragment_expression(in.opcode);
        if (!expression) return std::format("fragment opcode {:#x}", in.opcode);
        body += std::format("    float4 a = {};\n    float4 b = {};\n    float4 c = {};\n",
                            fragment_source(in, in.sources[0]), fragment_source(in, in.sources[1]),
                            fragment_source(in, in.sources[2]));
        std::string value = with_precision(in.precision, "float4(" + *expression + ")");
        if (in.saturate) value = "clamp(" + value + ", 0.0, 1.0)";
        body += std::format("    float4 value = {};\n", value);
        const std::string destination =
            std::format("{}[{}]", in.destination_half ? "h" : "r", in.destination);
        for (int i = 0; i < 4; ++i) {
            if ((in.mask & (1u << i)) == 0) continue;
            const char component = k_components[i];
            std::string writes;
            if (in.set_condition) writes += std::format(" cc.{0} = value.{0};", component);
            if (!in.no_destination) {
                writes +=
                    in.destination_half
                        ? std::format(" {0}.{1} = float(half(value.{1}));", destination, component)
                        : std::format(" {0}.{1} = value.{1};", destination, component);
            }
            if (!writes.empty())
                body += std::format("    if (pass.{}) {{{} }}\n", component, writes);
        }
        body += "    }\n";
    }
    return std::nullopt;
}

// Discards the fragment unless its alpha, as an 8-bit value, passes the alpha test.
std::string alpha_test(const std::optional<rsx::AlphaTest>& test, bool half_output) {
    if (!test) return "";
    const char* comparison = "";
    switch (test->compare) {
    case rsx::Compare::never: comparison = "false &&"; break;
    case rsx::Compare::less: comparison = "<"; break;
    case rsx::Compare::equal: comparison = "=="; break;
    case rsx::Compare::less_equal: comparison = "<="; break;
    case rsx::Compare::greater: comparison = ">"; break;
    case rsx::Compare::not_equal: comparison = "!="; break;
    case rsx::Compare::greater_equal: comparison = ">="; break;
    case rsx::Compare::always: return "";
    }
    return std::format("    uint alpha = uint(rint(saturate({}[0].w) * 255.0));\n"
                       "    if (!(alpha {} {}u)) discard_fragment();\n",
                       half_output ? "h" : "r", comparison, test->reference);
}

} // namespace

MslTranslation translate_to_msl(const rsx::DrawRequest& request) {
    using namespace rsx;
    const auto& vertex_program = request.vertex_program.instructions;
    const auto& fragment_program = request.fragment_program;
    const auto& textures = request.textures;
    const bool flat_shading = request.flat_shading;
    MslTranslation translation;
    std::string vertex_body;
    std::string fragment_body;
    if (auto missing = translate_vertex(vertex_program, vertex_body)) {
        translation.error = *missing + " can't be translated to Metal yet";
        return translation;
    }
    if (auto missing = translate_fragment(fragment_program, textures, fragment_body)) {
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
    std::string texture_parameters;
    for (u32 unit = 0; unit < textures.size(); ++unit) {
        if (!textures[unit]) continue;
        texture_parameters += std::format(",\n                              texture2d<float> t{0} "
                                          "[[texture({0})]], sampler s{0} [[sampler({0})]]",
                                          unit);
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
                            constant float4* constants [[buffer(1)]],
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

fragment float4 fragment_main(Varyings in [[stage_in]]{}) {{
    float4 f[{}] = {{}};
{}    float4 r[{}] = {{}};
    float4 h[{}] = {{}};
    float4 cc = float4(0.0);
{}{}    return {};
}}
)",
        varyings, vertex::k_input_count, vertex::k_input_count, vertex::k_input_count,
        vertex::k_temp_count, vertex::k_output_count, vertex_body, copy_outputs, texture_parameters,
        fragment::k_input_count, fragment_inputs, fragment::k_temp_count, fragment::k_temp_count,
        fragment_body, alpha_test(request.alpha_test, fragment_program.half_output),
        fragment_program.half_output ? "h[0]" : "r[0]");
    return translation;
}

} // namespace gpu
