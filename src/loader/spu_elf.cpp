#include "loader/spu_elf.h"

#include "cpu/spu/state.h"
#include "loader/elf.h"

#include <format>

namespace loader {

namespace {

// ELF32 header and program header offsets (System V ABI, "ELF Header" and "Program Header").
constexpr u32 k_class_offset = 4;
constexpr u32 k_data_offset = 5;
constexpr u32 k_machine_offset = 18;
constexpr u32 k_entry_offset = 24;
constexpr u32 k_phoff_offset = 28;
constexpr u32 k_phentsize_offset = 42;
constexpr u32 k_phnum_offset = 44;
constexpr u32 k_program_header_size = 32;

constexpr u8 k_class_32 = 1;
constexpr u8 k_data_big_endian = 2;

} // namespace

SpuElf read_spu_elf(const Memory& memory, u32 address) {
    const bool is_elf = memory.read<u32>(address) == 0x7f45'4c46; // "\x7fELF"
    if (!is_elf || memory.read<u8>(address + k_class_offset) != k_class_32 ||
        memory.read<u8>(address + k_data_offset) != k_data_big_endian) {
        throw LoadError("not a 32-bit big-endian ELF file");
    }
    if (memory.read<u16>(address + k_machine_offset) != k_elf_machine_spu) {
        throw LoadError("not an SPU program");
    }
    if (memory.read<u16>(address + k_phentsize_offset) != k_program_header_size) {
        throw LoadError("unexpected program header size");
    }

    SpuElf elf;
    elf.entry = memory.read<u32>(address + k_entry_offset);
    const u32 table = address + memory.read<u32>(address + k_phoff_offset);
    const u16 count = memory.read<u16>(address + k_phnum_offset);

    for (u32 i = 0; i < count; ++i) {
        const u32 header = table + i * k_program_header_size;
        if (memory.read<u32>(header) != k_segment_load) continue;
        SpuElfSegment segment;
        segment.file_offset = memory.read<u32>(header + 4);
        segment.local_address = memory.read<u32>(header + 8);
        segment.file_size = memory.read<u32>(header + 16);
        segment.memory_size = memory.read<u32>(header + 20);
        if (segment.file_size > segment.memory_size ||
            u64{segment.local_address} + segment.memory_size > spu::k_local_store_size) {
            throw LoadError(std::format("segment {} doesn't fit in local store", i));
        }
        elf.segments.push_back(segment);
    }
    if (elf.entry >= spu::k_local_store_size) throw LoadError("entry point outside local store");
    return elf;
}

} // namespace loader
