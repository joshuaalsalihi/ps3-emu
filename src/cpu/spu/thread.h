#pragma once

#include "common/types.h"
#include "core/clock.h"
#include "core/state.h"
#include "cpu/spu/local_store.h"
#include "cpu/spu/state.h"

#include <optional>
#include <string>

namespace spu {

// Why the interpreter handed control back.
enum class StopReason {
    stop,    // stop or stopd: the SPU program signals the PPU; pc is after the instruction
    halt,    // a halt instruction (heq, hgt, ...) fired; pc is on it
    channel, // rdch, rchcnt or wrch: the channel code must perform it; pc is on it
    fault,   // illegal instruction; pc is on it
};

struct Stop {
    StopReason reason = StopReason::fault;
    std::string message;
    u32 signal = 0; // stop: the signal type

    void serialize(state::Archive& archive) { archive(reason, message, signal); }
};

// The signal type stopd reports. The SPU ISA doesn't give one; psdevwiki ("Synergistic
// Processing Unit (SPU)") says 0x3fff, stop's largest type.
constexpr u32 k_stopd_signal = 0x3fff;

// An SPU's execution context: registers, local store, and what instruction handlers need.
struct Thread {
    explicit Thread(VirtualClock& clock) : clock(clock) {}

    State state;
    LocalStore local_store;
    VirtualClock& clock;

    // Address of the next instruction. Set to pc + 4 before each instruction; branches change it.
    u32 next_pc = 0;
    // Set by a handler to make the interpreter stop after the current instruction.
    std::optional<Stop> stop;
    // An SPU event is pending: bisled branches when this is set.
    bool event_pending = false;

    Vector& gpr(u32 n) { return state.gpr[n]; }
    // The preferred word slot of a register: where scalar values and addresses live.
    u32 preferred(u32 n) const { return state.gpr[n].word(0); }

    void serialize(state::Archive& archive) {
        archive(state, local_store, next_pc, stop, event_pending);
    }
};

} // namespace spu
