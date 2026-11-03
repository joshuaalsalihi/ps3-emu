#pragma once

// Translates RSX shader programs (the shared IR) into Metal Shading Language, for the Metal
// backend. The generated source has two functions:
//  - vertex_main(vertex_id, inputs [buffer 0], constants [buffer 1], viewport [buffer 2]), where
//    inputs holds 16 float4 per vertex and the viewport is {scale, offset, target size};
//  - fragment_main(textures and samplers [n] for each unit n the program samples), whose result
//    is the color in R0. It also does the alpha test, discarding the fragments that fail it.
// The vertex function applies the RSX's viewport transform itself, then hands Metal clip
// coordinates that land on the same pixels.

#include "rsx/backend.h"

#include <optional>
#include <string>

namespace gpu {

struct MslTranslation {
    std::string source;
    std::optional<std::string> error; // an instruction we can't translate yet
};

// Translates the request's programs, with what else they depend on: flat shading, the textures
// (whether their coordinates are normalized, their LOD bias) and the alpha test.
MslTranslation translate_to_msl(const rsx::DrawRequest& request);

} // namespace gpu
