#pragma once

// The Metal backend (macOS): renders draws on the GPU. Shaders are translated to Metal Shading
// Language (gpu/msl.h); for each draw the render target is loaded from guest memory into a
// texture, drawn into, and written back, so guest memory always holds the result.
//
// The GPU computes with its own floating point, so frames can differ slightly from the reference
// backend's; tests compare them with a small tolerance. Clears are plain memory fills, done the
// reference backend's way.

#include "core/memory.h"
#include "gpu/reference_backend.h"
#include "rsx/backend.h"

#include <memory>
#include <string>

namespace gpu {

class MetalBackend : public rsx::Backend {
public:
    // Returns null, with `error` set, if this host has no Metal device.
    static std::unique_ptr<MetalBackend> create(Memory& memory, std::string& error);
    ~MetalBackend() override;

    void clear(const rsx::ClearRequest& request) override { clears_.clear(request); }
    std::optional<std::string> draw(const rsx::DrawRequest& request) override;

private:
    struct Device; // the Objective-C objects
    MetalBackend(Memory& memory, std::unique_ptr<Device> device);

    Memory& memory_;
    ReferenceBackend clears_;
    std::unique_ptr<Device> device_;
};

} // namespace gpu
