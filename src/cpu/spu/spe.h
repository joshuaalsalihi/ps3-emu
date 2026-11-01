#pragma once

// A Synergistic Processor Element (SPE): an SPU together with its memory flow controller (MFC),
// which owns the DMA engine and the channels.

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "cpu/spu/channels.h"
#include "cpu/spu/dma.h"
#include "cpu/spu/thread.h"

#include <iosfwd>
#include <optional>
#include <string>

namespace spu {

struct Spe {
    // `id` identifies the SPE's atomic reservations in `memory`; it must differ from every
    // other reservation owner (such as PPU thread IDs).
    Spe(u32 id, Memory& memory, VirtualClock& clock)
        : cpu(clock), dma(id, memory, cpu.local_store, clock), channels(dma, cpu.state, clock) {}

    Thread cpu;
    Dma dma;
    Channels channels;

    // If the SPU is stopped on a channel access that can't complete yet, the channel.
    std::optional<u32> blocked_channel();
};

// Why run() returned.
enum class SpeExit {
    budget,  // it used its instruction budget
    stalled, // a channel access must wait; pc stays on it, to be run again later
    stopped, // stop or stopd; pc is after it
    // wrch to SPU_WrOutIntrMbox, which interrupts the PPU so the kernel can take the value;
    // pc is after it
    interrupt,
    fault, // halt, illegal instruction, invalid channel or DMA error; pc is on the instruction
};

struct SpeRun {
    SpeExit exit = SpeExit::budget;
    u64 instructions = 0;
    u32 channel = 0;     // stalled: the channel
    u32 signal = 0;      // stopped: the stop instruction's signal type
    std::string message; // fault
};

// Runs the SPU for up to `budget` instructions, performing channel accesses as they come.
// If `trace` is set, each instruction and channel access is printed.
SpeRun run(Spe& spe, u64 budget, std::ostream* trace = nullptr);

} // namespace spu
