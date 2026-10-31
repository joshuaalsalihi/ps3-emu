#pragma once

#include "common/types.h"
#include "cpu/ppu/thread.h"

#include <iosfwd>

namespace ppu {

// Runs `thread` for up to `max_instructions`, one instruction at a time. Returns the number of
// instructions executed. Stops early when an instruction sets `thread.stop`: a syscall or HLE
// call leaves pc after the instruction, while traps and faults leave pc on the offending
// instruction.
//
// If `trace` is set, each instruction is printed before it executes.
u64 interpret(Thread& thread, u64 max_instructions, std::ostream* trace = nullptr);

} // namespace ppu
