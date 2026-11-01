#pragma once

// What the RSX's state model asks a GPU backend to do. The state model decodes registers into
// these requests; a backend only renders. Render targets live in guest memory, so a backend that
// renders elsewhere (the GPU) must write its results back before anyone reads them.

#include "common/types.h"

#include <optional>
#include <vector>

namespace rsx {

// Color surface formats (the low 5 bits of SURFACE_FORMAT). PSL1GHT, rsx/gcm_sys.h:
// GCM_SURFACE_*.
enum class ColorFormat : u32 {
    x8r8g8b8_z8r8g8b8 = 4, // the X byte reads as 0
    x8r8g8b8_o8r8g8b8 = 5, // the X byte reads as 1s
    a8r8g8b8 = 8,
};

// Depth formats (bits 5-7 of SURFACE_FORMAT). GCM_SURFACE_ZETA_*.
enum class DepthFormat : u32 { z16 = 1, z24s8 = 2 };

// A rectangle in pixels.
struct Rect {
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

struct ColorSurface {
    u32 address = 0; // guest address of pixel (0, 0)
    u32 pitch = 0;   // bytes per row
    ColorFormat format = ColorFormat::a8r8g8b8;
};

struct DepthSurface {
    u32 address = 0;
    u32 pitch = 0;
    DepthFormat format = DepthFormat::z24s8;
};

// Clears part of the render targets. Only the channels in `mask` change (method::k_clear_*).
struct ClearRequest {
    std::vector<ColorSurface> colors;
    std::optional<DepthSurface> depth;
    Rect area;
    u32 mask = 0;
    u32 color = 0;       // A8R8G8B8
    u32 depth_value = 0; // Z16 in the low 16 bits, or Z24 << 8 | S8
};

// A frame as it reaches the display: 8-bit RGB, top row first.
struct Frame {
    u64 number = 0; // counting from 1
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgb;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void clear(const ClearRequest& request) = 0;
    // Writes everything rendered so far back to guest memory.
    virtual void flush() {}
};

} // namespace rsx
