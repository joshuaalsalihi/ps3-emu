#pragma once

// The reference backend: renders in software, straight into guest memory, one pixel at a time.
// It is slow, but its results are exact and identical on every host, so its frames are the
// golden images other backends are checked against.

#include "core/memory.h"
#include "rsx/backend.h"

namespace gpu {

class ReferenceBackend : public rsx::Backend {
public:
    explicit ReferenceBackend(Memory& memory) : memory_(memory) {}

    void clear(const rsx::ClearRequest& request) override;
    std::optional<std::string> draw(const rsx::DrawRequest& request) override;

private:
    void write_pixel(const rsx::DrawRequest& request, u32 x, u32 y, rsx::Vec4 color);
    static u32 depth_value(const rsx::DepthTest& test, rsx::shader_float::Float z);
    u32 read_depth(const rsx::DepthTest& test, u32 x, u32 y) const;
    void write_depth(const rsx::DepthTest& test, u32 x, u32 y, u32 value);

    Memory& memory_;
};

// Renders nothing: for runs that only need the CPU side.
class NullBackend : public rsx::Backend {
public:
    void clear(const rsx::ClearRequest&) override {}
    std::optional<std::string> draw(const rsx::DrawRequest&) override { return std::nullopt; }
};

} // namespace gpu
