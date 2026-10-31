#pragma once

#include "common/types.h"

#include <string>

namespace ppu {

// Disassembles one instruction located at `pc` (needed for branch targets), e.g.
// "addi r3, r1, -8". Common extended mnemonics such as "li", "mr" and "blr" are used where
// they apply. Unknown instructions print as ".long 0x...".
std::string disassemble(u32 raw, u64 pc);

} // namespace ppu
