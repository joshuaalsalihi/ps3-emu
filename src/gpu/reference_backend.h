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

private:
    Memory& memory_;
};

// Renders nothing: for runs that only need the CPU side.
class NullBackend : public rsx::Backend {
public:
    void clear(const rsx::ClearRequest&) override {}
};

} // namespace gpu
