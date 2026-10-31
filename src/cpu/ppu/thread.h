#pragma once

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "cpu/ppu/state.h"

#include <optional>
#include <string>

namespace ppu {

// Why the interpreter handed control back.
enum class StopReason {
    syscall,  // `sc`: the kernel must handle a system call
    hle_call, // `hle`: an emulated library function must run
    trap,     // a trap instruction fired
    fault,    // illegal instruction or bad memory access
};

struct Stop {
    StopReason reason;
    std::string message;
};

// A guest PPU thread: its registers, plus what instruction handlers need to reach.
struct Thread {
    Thread(u32 id, Memory& memory, VirtualClock& clock) : id(id), memory(memory), clock(clock) {}

    u32 id;
    State state;
    Memory& memory;
    VirtualClock& clock;

    // Address of the next instruction. Set to pc + 4 before each instruction; branches change it.
    u64 next_pc = 0;
    // Set by a handler to make the interpreter stop after the current instruction.
    std::optional<Stop> stop;

    // The Cell's VMX "graphics rounding mode" (HID1[grap_mode]), a hardware setting rather than
    // a user register. The Cell BE Programming Handbook (v1.11, section 2.5.6 and A.3.3) says it
    // is enabled by default. UNVERIFIED: whether the PS3's system software leaves it enabled.
    bool vmx_graphics_mode = true;

    u64& gpr(u32 n) { return state.gpr[n]; }
    // (rA|0) from the manuals: register rA, or the value 0 when rA is r0.
    u64 gpr_or_zero(u32 n) const { return n == 0 ? 0 : state.gpr[n]; }
};

} // namespace ppu
