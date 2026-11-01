#include "gpu/reference_backend.h"

#include "rsx/methods.h"

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

} // namespace gpu
