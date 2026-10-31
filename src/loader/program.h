#pragma once

#include "common/types.h"
#include "core/memory.h"
#include "loader/elf.h"

#include <span>

namespace loader {

struct LoadedProgram {
    u64 entry; // address of the first instruction
    u64 toc;   // table of contents pointer, which the ABI keeps in r2
};

// Maps the ELF's LOAD segments into guest memory, copies their contents, and reads the entry
// point's function descriptor. Throws LoadError if the program doesn't fit or is malformed.
LoadedProgram load_program(const ElfFile& elf, std::span<const u8> file, Memory& memory);

} // namespace loader
