#pragma once

#include "common/types.h"
#include "cpu/spu/thread.h"

#include <iosfwd>

namespace spu {

// Runs `thread` for up to `max_instructions`, one instruction at a time. Returns the number of
// instructions executed. Stops early when an instruction sets `thread.stop`: stop and stopd
// leave pc after the instruction; halts, channel accesses and faults leave pc on it.
//
// If `trace` is set, each instruction is printed before it executes.
u64 interpret(Thread& thread, u64 max_instructions, std::ostream* trace = nullptr);

} // namespace spu
