#include "gpu/vertex_input.h"

#include "rsx/methods.h"

namespace gpu {

using namespace rsx::shader_float;
using rsx::Vec4;

namespace {

constexpr Float k_255 = 0x437f'0000; // 255.0

} // namespace

std::array<Vec4, rsx::vertex::k_input_count>
fetch_vertex(const Memory& memory, const rsx::DrawRequest& request, u32 index) {
    std::array<Vec4, rsx::vertex::k_input_count> inputs;
    for (u32 i = 0; i < inputs.size(); ++i) {
        inputs[i] = {k_zero, k_zero, k_zero, k_one};
        const auto& array = request.inputs[i];
        if (!array) continue;
        const u32 base = array->address + index * array->stride;
        for (u32 c = 0; c < array->size; ++c) {
            if (array->type == rsx::method::k_vertex_float) {
                inputs[i][c] = memory.read<u32>(base + 4 * c);
            } else {
                inputs[i][c] = divide(from_integer(memory.read<u8>(base + c)), k_255);
            }
        }
    }
    return inputs;
}

std::optional<u32> vertex_output_for(u32 input) {
    using namespace rsx;
    if (input == fragment::k_input_color0) return vertex::k_output_color0;
    if (input == fragment::k_input_color1) return vertex::k_output_color1;
    if (input == fragment::k_input_fog) return vertex::k_output_fog;
    if (input >= fragment::k_input_texcoord0 && input < fragment::k_input_texcoord0 + 10) {
        return vertex::k_output_texcoord0 + (input - fragment::k_input_texcoord0);
    }
    return std::nullopt;
}

std::vector<std::array<std::size_t, 3>> assemble(rsx::Primitive primitive, std::size_t count) {
    std::vector<std::array<std::size_t, 3>> triangles;
    switch (primitive) {
    case rsx::Primitive::triangles:
        for (std::size_t i = 0; i + 2 < count; i += 3) {
            triangles.push_back({i, i + 1, i + 2});
        }
        break;
    case rsx::Primitive::triangle_strip:
        // Every other triangle swaps its first two vertices, so all keep the same winding.
        for (std::size_t i = 2; i < count; ++i) {
            if (i % 2 == 0) {
                triangles.push_back({i - 2, i - 1, i});
            } else {
                triangles.push_back({i - 1, i - 2, i});
            }
        }
        break;
    case rsx::Primitive::triangle_fan:
        for (std::size_t i = 2; i < count; ++i) {
            triangles.push_back({0, i - 1, i});
        }
        break;
    // Quads and polygons become triangles in the same winding, each ending with the vertex
    // OpenGL takes flat-shaded values from: a quad's last, a quad strip quad's fourth, a
    // polygon's first. UNVERIFIED: the diagonal the RSX splits quads along.
    case rsx::Primitive::quads:
        for (std::size_t i = 0; i + 3 < count; i += 4) {
            triangles.push_back({i, i + 1, i + 3});
            triangles.push_back({i + 1, i + 2, i + 3});
        }
        break;
    case rsx::Primitive::quad_strip:
        // Quad i has corners 2i, 2i + 1, 2i + 3, 2i + 2, in that order around it.
        for (std::size_t i = 0; 2 * i + 3 < count; ++i) {
            triangles.push_back({2 * i, 2 * i + 1, 2 * i + 3});
            triangles.push_back({2 * i + 2, 2 * i, 2 * i + 3});
        }
        break;
    case rsx::Primitive::polygon:
        for (std::size_t i = 2; i < count; ++i) {
            triangles.push_back({i - 1, i, 0});
        }
        break;
    }
    return triangles;
}

} // namespace gpu
