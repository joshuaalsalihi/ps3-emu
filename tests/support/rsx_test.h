#pragma once

#include "core/clock.h"
#include "core/memory.h"
#include "gpu/reference_backend.h"
#include "rsx/methods.h"
#include "rsx/rsx.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <vector>

namespace test {

using rsx::Frame;
using rsx::Rsx;
namespace method = rsx::method;

// An RSX with the reference backend, and helpers that write commands into IO memory as libgcm
// would. Methods go to subchannel 0 (3D) unless told otherwise.
class RsxTest {
public:
    static constexpr u32 k_io = 0x1000'0000; // main memory mapped at IO offset 0
    static constexpr u32 k_start = 0x1000;   // where the commands go, as an IO offset

    RsxTest() {
        memory.map(k_io, Rsx::k_io_page_size, Memory::k_read_write);
        REQUIRE(rsx.initialize(k_io, Rsx::k_io_page_size));
        rsx.set_backend(&backend);
        rsx.on_frame = [this](const Frame& frame) { frames.push_back(frame); };
        set_get(k_start);
        set_put(k_start);
    }

    void set_get(u32 offset) { memory.write<u32>(Rsx::k_control_register + 4, offset); }
    void set_put(u32 offset) { memory.write<u32>(Rsx::k_control_register, offset); }

    // Appends raw command words and moves PUT past them.
    void push(std::initializer_list<u32> words) {
        for (const u32 word : words) {
            memory.write<u32>(k_io + end_, word);
            end_ += 4;
        }
        set_put(end_);
    }

    // Appends a command writing `values` to consecutive methods from `method`.
    void methods(u32 method, std::initializer_list<u32> values, u32 subchannel = 0) {
        push({static_cast<u32>(values.size()) << 18 | subchannel << 13 | method});
        for (const u32 value : values) {
            push({value});
        }
    }

    // A 16x8 render target at local offset 0, with room to spare in each 128-byte row.
    void set_surface(u32 format = 5) {
        methods(method::k_surface_clip_horizontal, {16u << 16, 8u << 16, format, 128, 0});
        methods(method::k_set_color_context_a, {method::k_context_local});
        methods(method::k_surface_color_target, {1});
    }

    u32 pixel(u32 x, u32 y) const { return memory.read<u32>(Rsx::k_local_base + y * 128 + x * 4); }

    VirtualClock clock;
    Memory memory;
    Rsx rsx{memory, clock};
    gpu::ReferenceBackend backend{memory};
    std::vector<Frame> frames;

private:
    u32 end_ = k_start;
};

} // namespace test
