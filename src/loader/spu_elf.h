#pragma once

// SPU executables: 32-bit big-endian ELF files whose segments load into an SPU's local store.
// Programs embed them in their own data and pass their address to sys_spu_image_import.

#include "common/types.h"
#include "core/memory.h"

#include <vector>

namespace loader {

constexpr u16 k_elf_machine_spu = 23; // EM_SPU

// A LOAD segment: `file_size` bytes from `file_offset` go to `local_address`, and the rest of
// `memory_size` is zero-filled.
struct SpuElfSegment {
    u32 file_offset = 0;
    u32 local_address = 0;
    u32 file_size = 0;
    u32 memory_size = 0;
};

struct SpuElf {
    u32 entry = 0;
    std::vector<SpuElfSegment> segments; // LOAD segments only
};

// Reads the header and LOAD segments of the SPU ELF at `address` in guest memory. Throws
// LoadError if it isn't a valid SPU executable, and MemoryFault if it isn't readable.
SpuElf read_spu_elf(const Memory& memory, u32 address);

} // namespace loader
