#pragma once

// Translates RSX shader programs (the shared IR) into Metal Shading Language, for the Metal
// backend. The generated source has two functions:
//  - vertex_main(vertex_id, inputs [buffer 0], constants [buffer 1], viewport [buffer 2]), where
//    inputs holds 16 float4 per vertex and the viewport is {scale, offset, target size};
//  - fragment_main, whose result is the color in R0.
// The vertex function applies the RSX's viewport transform itself, then hands Metal clip
// coordinates that land on the same pixels.

#include "rsx/fragment_program.h"
#include "rsx/vertex_program.h"

#include <optional>
#include <string>

namespace gpu {

struct MslTranslation {
    std::string source;
    std::optional<std::string> error; // an instruction we can't translate yet
};

MslTranslation translate_to_msl(const std::vector<rsx::vertex::Instruction>& vertex_program,
                                const rsx::fragment::Program& fragment_program, bool flat_shading);

} // namespace gpu
