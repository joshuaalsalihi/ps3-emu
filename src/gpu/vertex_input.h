#pragma once

// What every backend does the same way before rendering: reading vertex inputs from their arrays,
// grouping vertices into triangles, and matching fragment program inputs to vertex outputs.

#include "core/memory.h"
#include "rsx/backend.h"

#include <array>
#include <optional>
#include <vector>

namespace gpu {

// Reads vertex `index`'s inputs from their arrays; inputs without an array read 0, 0, 0, 1.
std::array<rsx::Vec4, rsx::vertex::k_input_count>
fetch_vertex(const Memory& memory, const rsx::DrawRequest& request, u32 index);

// Which vertex output each fragment program input interpolates, if any.
std::optional<u32> vertex_output_for(u32 input);

// The three positions in a draw's vertex list that make each triangle of `primitive`.
std::vector<std::array<std::size_t, 3>> assemble(rsx::Primitive primitive, std::size_t count);

} // namespace gpu
