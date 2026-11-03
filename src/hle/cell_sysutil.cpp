// cellSysutil: system utilities. For now only video output, which a program configures before
// it sets up its display buffers. Our virtual display is a 720p HDMI screen.
//
// Structure layouts: PSL1GHT's sysutil/video.h, whose video* functions wrap these.

#include "hle/linker.h"
#include "rsx/rsx.h"

#include <array>

namespace hle {

namespace {

using lv2::Kernel;
using lv2::PpuThread;

// UNVERIFIED: sysutil has its own error codes (CELL_VIDEO_OUT_ERROR_*), which we don't have yet.
constexpr s32 k_video_out_error = lv2::k_error_invalid;

struct Resolution {
    u8 id;
    u16 width;
    u16 height;
};

// VIDEO_RESOLUTION_*. PSL1GHT, sysutil/video.h.
constexpr std::array<Resolution, 8> k_resolutions = {{
    {1, 1920, 1080},
    {2, 1280, 720},
    {4, 720, 480},
    {5, 720, 576},
    {10, 1600, 1080},
    {11, 1440, 1080},
    {12, 1280, 1080},
    {13, 960, 1080},
}};

const Resolution* find_resolution(u32 id) {
    for (const Resolution& resolution : k_resolutions) {
        if (resolution.id == id) return &resolution;
    }
    return nullptr;
}

constexpr u8 k_state_enabled = 1;      // VIDEO_STATE_ENABLED
constexpr u8 k_scan_progressive = 1;   // VIDEO_SCANMODE_PROGRESSIVE
constexpr u16 k_refresh_59_94hz = 1;   // VIDEO_REFRESH_59_94HZ
constexpr u32 k_primary_video_out = 0; // VIDEO_PRIMARY

// s32 cellVideoOutGetState(u32 videoOut, u32 deviceIndex, videoState* state)
// videoState: {u8 state, u8 colorSpace, u8 padding[6], videoDisplayMode displayMode}, where
// videoDisplayMode is {u8 resolution, scanMode, conversion, aspect, padding[2], u16 refreshRates}.
void cell_video_out_get_state(Kernel& kernel, PpuThread& thread) {
    if (!kernel.rsx || thread.arg(0) != k_primary_video_out || thread.arg(1) != 0) {
        return thread.set_result(k_video_out_error);
    }
    const u32 state = thread.pointer_arg(2);
    const rsx::VideoMode& mode = kernel.rsx->video_mode;
    const std::array<u8, 16> bytes = {
        k_state_enabled, mode.format,        0, 0,           0, 0, 0, 0,
        mode.resolution, k_scan_progressive, 0, mode.aspect, 0, 0, 0, k_refresh_59_94hz};
    kernel.memory.write_bytes(state, bytes);
    thread.set_result(lv2::k_ok);
}

// s32 cellVideoOutGetResolution(u32 resolutionId, videoResolution* resolution)
// videoResolution: {u16 width, u16 height}.
void cell_video_out_get_resolution(Kernel& kernel, PpuThread& thread) {
    const Resolution* resolution = find_resolution(static_cast<u32>(thread.arg(0)));
    if (!resolution) return thread.set_result(k_video_out_error);
    kernel.memory.write<u16>(thread.pointer_arg(1), resolution->width);
    kernel.memory.write<u16>(thread.pointer_arg(1) + 2, resolution->height);
    thread.set_result(lv2::k_ok);
}

// s32 cellVideoOutGetResolutionAvailability(u32 videoOut, u32 resolutionId, u32 aspect,
//                                            u32 option)
// Returns 1 if the display accepts the resolution, else 0. UNVERIFIED: which modes a 720p HDMI
// display reports; we say 720p, 480p and 576p, in any aspect.
void cell_video_out_get_resolution_availability(Kernel&, PpuThread& thread) {
    const u32 id = static_cast<u32>(thread.arg(1));
    const bool available = thread.arg(0) == k_primary_video_out && (id == 2 || id == 4 || id == 5);
    thread.set_result(available ? 1 : 0);
}

// s32 cellVideoOutConfigure(u32 videoOut, videoConfiguration* config, void* option, u32 wait)
// videoConfiguration: {u8 resolution, u8 format, u8 aspect, u8 padding[9], u32 pitch}.
void cell_video_out_configure(Kernel& kernel, PpuThread& thread) {
    const u32 config = thread.pointer_arg(1);
    const u8 resolution = kernel.memory.read<u8>(config);
    if (!kernel.rsx || thread.arg(0) != k_primary_video_out || !find_resolution(resolution)) {
        return thread.set_result(k_video_out_error);
    }
    rsx::VideoMode& mode = kernel.rsx->video_mode;
    mode.resolution = resolution;
    mode.format = kernel.memory.read<u8>(config + 1);
    // VIDEO_ASPECT_AUTO (0) becomes the display's own aspect.
    const u8 aspect = kernel.memory.read<u8>(config + 2);
    mode.aspect = aspect == 0 ? 2 : aspect; // VIDEO_ASPECT_16_9
    mode.pitch = kernel.memory.read<u32>(config + 12);
    thread.set_result(lv2::k_ok);
}

// s32 cellVideoOutGetConfiguration(u32 videoOut, videoConfiguration* config, void* option)
void cell_video_out_get_configuration(Kernel& kernel, PpuThread& thread) {
    if (!kernel.rsx || thread.arg(0) != k_primary_video_out) {
        return thread.set_result(k_video_out_error);
    }
    const u32 config = thread.pointer_arg(1);
    const rsx::VideoMode& mode = kernel.rsx->video_mode;
    const std::array<u8, 12> bytes = {mode.resolution, mode.format, mode.aspect};
    kernel.memory.write_bytes(config, bytes);
    kernel.memory.write<u32>(config + 12, mode.pitch);
    thread.set_result(lv2::k_ok);
}

constexpr FunctionInfo k_functions[] = {
    {"cellVideoOutGetState", cell_video_out_get_state},
    {"cellVideoOutGetResolution", cell_video_out_get_resolution},
    {"cellVideoOutGetResolutionAvailability", cell_video_out_get_resolution_availability},
    {"cellVideoOutConfigure", cell_video_out_configure},
    {"cellVideoOutGetConfiguration", cell_video_out_get_configuration},
};

} // namespace

std::span<const FunctionInfo> cell_sysutil_functions() {
    return k_functions;
}

} // namespace hle
