#pragma once

#include "common/types.h"

#include <string>

namespace spu {

// Disassembles one instruction located at `pc` (needed for branch targets), in spu-as syntax,
// e.g. "a $3, $4, $5". Unknown instructions print as ".long 0x...".
std::string disassemble(u32 raw, u32 pc);

} // namespace spu
