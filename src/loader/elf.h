#pragma once

#include "common/types.h"

#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace loader {

// Thrown when a file is not a valid PS3 PPU executable.
class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ELF file types (e_type).
constexpr u16 k_elf_type_exec = 2;
constexpr u16 k_elf_type_dyn = 3;
constexpr u16 k_elf_type_prx = 0xffa4; // psdevwiki, "PRX": ET_SCE_PPURELEXEC

// Machine types (e_machine).
constexpr u16 k_elf_machine_ppc64 = 21;

// Segment types (p_type).
constexpr u32 k_segment_null = 0;
constexpr u32 k_segment_load = 1;
constexpr u32 k_segment_dynamic = 2;
constexpr u32 k_segment_interp = 3;
constexpr u32 k_segment_note = 4;
constexpr u32 k_segment_phdr = 6;
constexpr u32 k_segment_tls = 7;
constexpr u32 k_segment_proc_param = 0x60000001; // psdevwiki, "ELF": sys_process_param_t
constexpr u32 k_segment_prx_param = 0x60000002;  // psdevwiki, "ELF": sys_prx_param_t
constexpr u32 k_segment_ppu_rela = 0x700000a4;   // psdevwiki, "PRX": relocations

// Segment permission flags (p_flags). PS3 files also set SPU/RSX bits above these.
constexpr u32 k_segment_flag_x = 1;
constexpr u32 k_segment_flag_w = 2;
constexpr u32 k_segment_flag_r = 4;

// One entry of the program header table: a region of the file mapped into memory.
struct ElfSegment {
    u32 type = 0;
    u32 flags = 0;
    u64 file_offset = 0;
    u64 vaddr = 0;
    u64 file_size = 0;
    u64 mem_size = 0;
    u64 align = 0;
};

struct ElfFile {
    u16 type = 0;
    u16 machine = 0;
    // On the PS3 the entry point is the address of a function descriptor
    // (code address + TOC pointer), not the address of the first instruction.
    u64 entry = 0;
    std::vector<ElfSegment> segments;
};

// Parses a big-endian ELF64 PPU executable. Throws LoadError if the file is invalid.
ElfFile parse_elf(std::span<const u8> data);

std::string elf_type_name(u16 type);
std::string segment_type_name(u32 type);
std::string segment_flags_string(u32 flags); // e.g. "R-X"

// Human-readable summary of the header and segment table.
std::string describe_elf(const ElfFile& elf);

} // namespace loader
