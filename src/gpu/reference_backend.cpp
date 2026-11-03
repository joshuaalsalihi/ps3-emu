#include "gpu/reference_backend.h"

#include "gpu/rasterizer.h"
#include "gpu/texture.h"
#include "gpu/vertex_input.h"
#include "rsx/methods.h"
#include "rsx/shader_ops.h"

#include <map>

namespace gpu {

namespace {

// The bits of an A8R8G8B8 pixel that a clear mask lets change.
u32 color_write_mask(u32 mask) {
    u32 bits = 0;
    if (mask & rsx::method::k_clear_alpha) bits |= 0xff00'0000;
    if (mask & rsx::method::k_clear_red) bits |= 0x00ff'0000;
    if (mask & rsx::method::k_clear_green) bits |= 0x0000'ff00;
    if (mask & rsx::method::k_clear_blue) bits |= 0x0000'00ff;
    return bits;
}

// The X byte of the X8R8G8B8 formats. UNVERIFIED: what the RSX stores there.
u32 fixed_x_byte(rsx::ColorFormat format) {
    switch (format) {
    case rsx::ColorFormat::x8r8g8b8_z8r8g8b8: return 0x0000'0000;
    case rsx::ColorFormat::x8r8g8b8_o8r8g8b8: return 0xff00'0000;
    case rsx::ColorFormat::a8r8g8b8: break;
    }
    return 0;
}

} // namespace

void ReferenceBackend::clear(const rsx::ClearRequest& request) {
    const rsx::Rect& area = request.area;

    for (const rsx::ColorSurface& surface : request.colors) {
        const bool has_alpha = surface.format == rsx::ColorFormat::a8r8g8b8;
        u32 bits = color_write_mask(request.mask) & (has_alpha ? 0xffff'ffff : 0x00ff'ffff);
        if (bits == 0) continue;
        u32 value = request.color;
        if (!has_alpha) {
            // The X byte always takes its fixed value.
            bits |= 0xff00'0000;
            value = (value & 0x00ff'ffff) | fixed_x_byte(surface.format);
        }
        for (u32 y = area.y; y < area.y + area.height; ++y) {
            for (u32 x = area.x; x < area.x + area.width; ++x) {
                const u32 address = surface.address + y * surface.pitch + x * 4;
                memory_.write<u32>(address, (memory_.read<u32>(address) & ~bits) | (value & bits));
            }
        }
    }

    const bool depth = (request.mask & rsx::method::k_clear_depth) != 0;
    const bool stencil = (request.mask & rsx::method::k_clear_stencil) != 0;
    if (!request.depth || (!depth && !stencil)) return;
    const rsx::DepthSurface& surface = *request.depth;
    for (u32 y = area.y; y < area.y + area.height; ++y) {
        for (u32 x = area.x; x < area.x + area.width; ++x) {
            if (surface.format == rsx::DepthFormat::z16) {
                if (!depth) continue;
                memory_.write<u16>(surface.address + y * surface.pitch + x * 2,
                                   static_cast<u16>(request.depth_value));
                continue;
            }
            const u32 address = surface.address + y * surface.pitch + x * 4;
            const u32 keep = (depth ? 0 : 0xffff'ff00) | (stencil ? 0 : 0xff);
            memory_.write<u32>(address,
                               (memory_.read<u32>(address) & keep) | (request.depth_value & ~keep));
        }
    }
}

// ---- Drawing ----

namespace {

using namespace rsx::shader_float;
using rsx::Vec4;

constexpr Float k_255 = 0x437f'0000; // 255.0
constexpr Float k_16 = 0x4180'0000;  // 16.0, the subpixel scale
constexpr Float k_half = 0x3f00'0000;
// Window coordinates beyond this many subpixels would need clipping first.
constexpr s64 k_max_subpixels = s64{1} << 24;

struct ShadedVertex {
    std::array<Vec4, rsx::vertex::k_output_count> outputs;
    FixedPoint window;
    Float window_z = k_zero;
    Float inverse_w = k_one;
};

struct Triangle {
    std::array<const ShadedVertex*, 3> vertices;
    Float inverse_area = k_one; // 1 / the total of the rasterizer's weights
};

// The fragment program's inputs that come from vertex outputs: fragment input, vertex output.
using Varyings = std::vector<std::pair<u32, u32>>;

struct QuadInputs {
    rsx::fragment::Quad<rsx::fragment::Inputs> inputs{};
    std::array<Float, 4> depth{}; // each lane's window z
};

// Each lane's inputs, from the triangle's vertices: window z varies linearly across the
// screen, the other values with perspective correction (each vertex's share, divided by its w).
QuadInputs interpolate(const rsx::DrawRequest& request, const Triangle& triangle,
                       const Varyings& varyings, const Quad& quad) {
    const auto& v = triangle.vertices;
    QuadInputs result;
    for (u32 lane = 0; lane < 4; ++lane) {
        rsx::fragment::Inputs& inputs = result.inputs[lane];
        inputs[rsx::fragment::k_input_position] = {add(from_integer(quad.x + lane % 2), k_half),
                                                   add(from_integer(quad.y + lane / 2), k_half),
                                                   k_zero, k_one};
        std::array<Float, 3> q{};
        Float sum = k_zero;
        for (int i = 0; i < 3; ++i) {
            const Float share =
                multiply(from_integer(quad.weights[lane][i]), triangle.inverse_area);
            q[i] = multiply(share, v[i]->inverse_w);
            sum = add(sum, q[i]);
            result.depth[lane] = multiply_add(share, v[i]->window_z, result.depth[lane]);
        }
        const Float inverse_sum = reciprocal(sum);
        for (const auto& [input, output] : varyings) {
            for (int c = 0; c < 4; ++c) {
                if (request.flat_shading) {
                    // UNVERIFIED: the last vertex provides flat-shaded values.
                    inputs[input][c] = v[2]->outputs[output][c];
                    continue;
                }
                Float value = k_zero;
                for (int i = 0; i < 3; ++i) {
                    value = multiply_add(q[i], v[i]->outputs[output][c], value);
                }
                inputs[input][c] = multiply(value, inverse_sum);
            }
        }
    }
    return result;
}

// A color channel clamped to 0..1 and rounded to 8 bits. UNVERIFIED: to nearest.
u32 to_8_bits(Float channel) {
    return static_cast<u32>(to_integer(multiply(rsx::shader_ops::saturate(channel), k_255)));
}

// An A8R8G8B8 value as red, green, blue and alpha in 0..1.
Vec4 unpack(u32 argb) {
    const auto channel = [&](u32 shift) {
        return divide(from_integer((argb >> shift) & 0xff), k_255);
    };
    return {channel(16), channel(8), channel(0), channel(24)};
}

Float factor_value(rsx::BlendFactor factor, int channel, const Vec4& source,
                   const Vec4& destination, const Vec4& constant) {
    using rsx::BlendFactor;
    const auto one_minus = [](Float value) { return add(k_one, value ^ rsx::shader_ops::k_sign); };
    switch (factor) {
    case BlendFactor::zero: return k_zero;
    case BlendFactor::one: return k_one;
    case BlendFactor::source_color: return source[channel];
    case BlendFactor::one_minus_source_color: return one_minus(source[channel]);
    case BlendFactor::source_alpha: return source[3];
    case BlendFactor::one_minus_source_alpha: return one_minus(source[3]);
    case BlendFactor::destination_alpha: return destination[3];
    case BlendFactor::one_minus_destination_alpha: return one_minus(destination[3]);
    case BlendFactor::destination_color: return destination[channel];
    case BlendFactor::one_minus_destination_color: return one_minus(destination[channel]);
    case BlendFactor::source_alpha_saturate:
        return channel == 3 ? k_one : minimum(source[3], one_minus(destination[3]));
    case BlendFactor::constant_color: return constant[channel];
    case BlendFactor::one_minus_constant_color: return one_minus(constant[channel]);
    case BlendFactor::constant_alpha: return constant[3];
    case BlendFactor::one_minus_constant_alpha: return one_minus(constant[3]);
    }
    return k_zero;
}

// OpenGL's blending, on colors clamped to 0..1. A target without alpha reads its alpha as 1.
// UNVERIFIED: the RSX blends 8-bit targets in fixed point; this is exact arithmetic.
Vec4 blend(const rsx::Blend& blend, const Vec4& color, Vec4 destination, bool has_alpha) {
    using rsx::BlendEquation;
    if (!has_alpha) destination[3] = k_one;
    Vec4 source;
    for (int i = 0; i < 4; ++i) {
        source[i] = rsx::shader_ops::saturate(color[i]);
    }
    const Vec4 constant = unpack(blend.color);
    Vec4 result;
    for (int i = 0; i < 4; ++i) {
        const bool alpha = i == 3;
        const Float s =
            multiply(source[i], factor_value(alpha ? blend.source_alpha : blend.source_rgb, i,
                                             source, destination, constant));
        const Float d = multiply(
            destination[i], factor_value(alpha ? blend.destination_alpha : blend.destination_rgb, i,
                                         source, destination, constant));
        switch (alpha ? blend.equation_alpha : blend.equation_rgb) {
        case BlendEquation::add: result[i] = add(s, d); break;
        case BlendEquation::subtract: result[i] = add(s, d ^ rsx::shader_ops::k_sign); break;
        case BlendEquation::reverse_subtract:
            result[i] = add(d, s ^ rsx::shader_ops::k_sign);
            break;
        // MIN and MAX ignore the factors.
        case BlendEquation::minimum: result[i] = minimum(source[i], destination[i]); break;
        case BlendEquation::maximum: result[i] = maximum(source[i], destination[i]); break;
        }
    }
    return result;
}

bool is_culled(const rsx::DrawRequest& request, s64 signed_area) {
    // A positive area is clockwise on screen.
    const bool front = request.front_ccw ? signed_area < 0 : signed_area > 0;
    switch (request.cull) {
    case rsx::Cull::none: return false;
    case rsx::Cull::front: return front;
    case rsx::Cull::back: return !front;
    case rsx::Cull::front_and_back: return true;
    }
    return false;
}

bool passes(rsx::Compare compare, u32 incoming, u32 stored) {
    switch (compare) {
    case rsx::Compare::never: return false;
    case rsx::Compare::less: return incoming < stored;
    case rsx::Compare::equal: return incoming == stored;
    case rsx::Compare::less_equal: return incoming <= stored;
    case rsx::Compare::greater: return incoming > stored;
    case rsx::Compare::not_equal: return incoming != stored;
    case rsx::Compare::greater_equal: return incoming >= stored;
    case rsx::Compare::always: return true;
    }
    return false;
}

} // namespace

std::optional<std::string> ReferenceBackend::draw(const rsx::DrawRequest& request) {
    // Shade each vertex once, then place it in window coordinates.
    std::map<u32, ShadedVertex> shaded;
    for (const u32 index : request.vertices) {
        if (shaded.contains(index)) continue;
        const rsx::vertex::Result result =
            rsx::vertex::run(request.vertex_program, fetch_vertex(memory_, request, index));
        if (result.error) return result.error;

        ShadedVertex vertex;
        vertex.outputs = result.outputs;
        const Vec4& position = result.outputs[rsx::vertex::k_output_position];
        if (!less(k_zero, position[3])) {
            return "vertices with w <= 0 need clipping, which is not supported yet";
        }
        vertex.inverse_w = reciprocal(position[3]);
        const auto window = [&](int axis) {
            const Float ndc = multiply(position[axis], vertex.inverse_w);
            return multiply_add(ndc, request.viewport_scale[axis], request.viewport_offset[axis]);
        };
        vertex.window = {to_integer(multiply(window(0), k_16)),
                         to_integer(multiply(window(1), k_16))};
        vertex.window_z = window(2);
        if (std::abs(vertex.window.x) > k_max_subpixels ||
            std::abs(vertex.window.y) > k_max_subpixels) {
            return "vertices far outside the viewport need clipping, which is not supported yet";
        }
        shaded[index] = vertex;
    }

    Varyings varyings;
    for (u32 input = 0; input < rsx::fragment::k_input_count; ++input) {
        if (const auto output = vertex_output_for(input)) varyings.emplace_back(input, *output);
    }

    const TextureSampler sampler(memory_, request);
    std::optional<std::string> error;
    for (const auto& corners : assemble(request.primitive, request.vertices.size())) {
        Triangle triangle;
        std::array<FixedPoint, 3> window{};
        for (int i = 0; i < 3; ++i) {
            triangle.vertices[i] = &shaded.at(request.vertices[corners[i]]);
            window[i] = triangle.vertices[i]->window;
        }
        const s64 area = signed_area(window);
        if (area == 0 || is_culled(request, area)) continue;
        triangle.inverse_area = reciprocal(from_integer(std::abs(area)));

        rasterize(window, request.area, [&](const Quad& quad) {
            if (error) return;
            const QuadInputs inputs = interpolate(request, triangle, varyings, quad);

            u32 visible = quad.covered;
            std::array<u32, 4> depth_values{};
            for (u32 lane = 0; lane < 4 && request.depth_test; ++lane) {
                if ((visible & (1u << lane)) == 0) continue;
                depth_values[lane] = depth_value(*request.depth_test, inputs.depth[lane]);
                const u32 stored =
                    read_depth(*request.depth_test, quad.x + lane % 2, quad.y + lane / 2);
                if (!passes(request.depth_test->compare, depth_values[lane], stored)) {
                    visible &= ~(1u << lane);
                }
            }
            if (visible == 0) return;

            const rsx::fragment::QuadResult result =
                rsx::fragment::run(request.fragment_program, inputs.inputs, &sampler);
            if (result.error) {
                error = result.error;
                return;
            }
            visible &= ~result.killed;
            for (u32 lane = 0; lane < 4; ++lane) {
                if ((visible & (1u << lane)) == 0) continue;
                if (request.alpha_test &&
                    !passes(request.alpha_test->compare, to_8_bits(result.colors[lane][3]),
                            request.alpha_test->reference)) {
                    continue;
                }
                const u32 x = quad.x + lane % 2;
                const u32 y = quad.y + lane / 2;
                write_pixel(request, x, y, result.colors[lane]);
                if (request.depth_test && request.depth_test->write) {
                    write_depth(*request.depth_test, x, y, depth_values[lane]);
                }
            }
        });
        if (error) return error;
    }
    return std::nullopt;
}

// Window z, clamped to 0..1, as a depth buffer value. UNVERIFIED: the RSX's clamping and
// rounding (here to nearest).
u32 ReferenceBackend::depth_value(const rsx::DepthTest& test, Float z) {
    const u32 maximum = test.surface.format == rsx::DepthFormat::z16 ? 0xffff : 0xff'ffff;
    const Float clamped = rsx::shader_ops::saturate(z);
    return static_cast<u32>(to_integer(multiply(clamped, from_integer(maximum))));
}

u32 ReferenceBackend::read_depth(const rsx::DepthTest& test, u32 x, u32 y) const {
    const rsx::DepthSurface& surface = test.surface;
    if (surface.format == rsx::DepthFormat::z16) {
        return memory_.read<u16>(surface.address + y * surface.pitch + x * 2);
    }
    return memory_.read<u32>(surface.address + y * surface.pitch + x * 4) >> 8;
}

// Z24S8 keeps its stencil byte.
void ReferenceBackend::write_depth(const rsx::DepthTest& test, u32 x, u32 y, u32 value) {
    const rsx::DepthSurface& surface = test.surface;
    if (surface.format == rsx::DepthFormat::z16) {
        memory_.write<u16>(surface.address + y * surface.pitch + x * 2, static_cast<u16>(value));
        return;
    }
    const u32 address = surface.address + y * surface.pitch + x * 4;
    memory_.write<u32>(address, value << 8 | (memory_.read<u32>(address) & 0xff));
}

// Blends a color with the pixel, converts it to 8 bits per channel and writes the channels the
// color mask allows.
void ReferenceBackend::write_pixel(const rsx::DrawRequest& request, u32 x, u32 y, Vec4 color) {
    const u32 address = request.target.address + y * request.target.pitch + x * 4;
    if (request.blend) {
        color = blend(*request.blend, color, unpack(memory_.read<u32>(address)),
                      request.target.format == rsx::ColorFormat::a8r8g8b8);
    }
    u32 value = to_8_bits(color[3]) << 24 | to_8_bits(color[0]) << 16 | to_8_bits(color[1]) << 8 |
                to_8_bits(color[2]);
    u32 bits = 0;
    const u32 mask = request.color_mask;
    if (mask & 0x0100'0000) bits |= 0xff00'0000;
    if (mask & 0x0001'0000) bits |= 0x00ff'0000;
    if (mask & 0x0000'0100) bits |= 0x0000'ff00;
    if (mask & 0x0000'0001) bits |= 0x0000'00ff;
    if (request.target.format != rsx::ColorFormat::a8r8g8b8) {
        bits |= 0xff00'0000;
        value = (value & 0x00ff'ffff) | fixed_x_byte(request.target.format);
    }
    memory_.write<u32>(address, (memory_.read<u32>(address) & ~bits) | (value & bits));
}

} // namespace gpu
