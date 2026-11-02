#include "gpu/reference_backend.h"

#include "gpu/rasterizer.h"
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
    Float inverse_w = k_one;
};

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
            const Float pixels =
                multiply_add(ndc, request.viewport_scale[axis], request.viewport_offset[axis]);
            return to_integer(multiply(pixels, k_16));
        };
        vertex.window = {window(0), window(1)};
        if (std::abs(vertex.window.x) > k_max_subpixels ||
            std::abs(vertex.window.y) > k_max_subpixels) {
            return "vertices far outside the viewport need clipping, which is not supported yet";
        }
        shaded[index] = vertex;
    }

    // The fragment program's inputs that come from vertex outputs.
    std::vector<std::pair<u32, u32>> varyings; // fragment input, vertex output
    for (u32 input = 0; input < rsx::fragment::k_input_count; ++input) {
        if (const auto output = vertex_output_for(input)) varyings.emplace_back(input, *output);
    }

    std::optional<std::string> error;
    s64 cached_area = 0;
    Float inverse_area = k_zero;
    for (const auto& triangle : assemble(request.primitive, request.vertices.size())) {
        std::array<const ShadedVertex*, 3> v{};
        for (int i = 0; i < 3; ++i) {
            v[i] = &shaded.at(request.vertices[triangle[i]]);
        }
        rasterize({v[0]->window, v[1]->window, v[2]->window}, request.area,
                  [&](u32 x, u32 y, const std::array<s64, 3>& weights, s64 area) {
                      if (error) return;
                      std::array<Vec4, rsx::fragment::k_input_count> inputs{};
                      inputs[rsx::fragment::k_input_position] = {add(from_integer(x), k_half),
                                                                 add(from_integer(y), k_half),
                                                                 k_zero, k_one};

                      // Perspective-correct weights: each vertex's share, divided by its w.
                      if (area != cached_area) {
                          cached_area = area;
                          inverse_area = reciprocal(from_integer(area));
                      }
                      std::array<Float, 3> q{};
                      Float sum = k_zero;
                      for (int i = 0; i < 3; ++i) {
                          q[i] = multiply(multiply(from_integer(weights[i]), inverse_area),
                                          v[i]->inverse_w);
                          sum = add(sum, q[i]);
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

                      const rsx::fragment::Result result =
                          rsx::fragment::run(request.fragment_program, inputs);
                      if (result.error) {
                          error = result.error;
                          return;
                      }
                      write_pixel(request, x, y, result.color);
                  });
        if (error) return error;
    }
    return std::nullopt;
}

// Converts a color to 8 bits per channel (clamped, rounded to nearest) and writes the channels
// the color mask allows.
void ReferenceBackend::write_pixel(const rsx::DrawRequest& request, u32 x, u32 y,
                                   const Vec4& color) {
    const auto channel = [&](int i) {
        return static_cast<u32>(to_integer(multiply(rsx::shader_ops::saturate(color[i]), k_255)));
    };
    u32 value = channel(3) << 24 | channel(0) << 16 | channel(1) << 8 | channel(2);
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
    const u32 address = request.target.address + y * request.target.pitch + x * 4;
    memory_.write<u32>(address, (memory_.read<u32>(address) & ~bits) | (value & bits));
}

} // namespace gpu
