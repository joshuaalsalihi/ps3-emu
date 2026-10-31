#include "loader/program.h"

#include <format>
#include <map>

namespace loader {

namespace {

u64 align_down(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}

u64 align_up(u64 value, u64 alignment) {
    return align_down(value + alignment - 1, alignment);
}

u8 memory_permissions(const ElfSegment& segment) {
    u8 permissions = 0;
    // Executable code must be readable for the interpreter to fetch it.
    if (segment.flags & (k_segment_flag_r | k_segment_flag_x)) permissions |= Memory::k_read;
    if (segment.flags & k_segment_flag_w) permissions |= Memory::k_write;
    return permissions;
}

} // namespace

LoadedProgram load_program(const ElfFile& elf, std::span<const u8> file, Memory& memory) {
    // Segments can share a page, so collect each page's combined permissions first.
    std::map<u64, u8> page_permissions;
    for (const ElfSegment& segment : elf.segments) {
        if (segment.type != k_segment_load || segment.mem_size == 0) continue;
        if (segment.vaddr >= Memory::k_size || segment.mem_size > Memory::k_size - segment.vaddr) {
            throw LoadError(
                std::format("segment at {:#x} is outside the 4 GB address space", segment.vaddr));
        }
        const u64 first = align_down(segment.vaddr, Memory::k_page_size);
        const u64 end = align_up(segment.vaddr + segment.mem_size, Memory::k_page_size);
        for (u64 page = first; page < end; page += Memory::k_page_size) {
            page_permissions[page] |= memory_permissions(segment);
        }
    }

    // Map everything writable while copying, then apply the real permissions.
    for (const auto& [page, permissions] : page_permissions) {
        memory.map(page, Memory::k_page_size, Memory::k_read_write);
    }
    for (const ElfSegment& segment : elf.segments) {
        if (segment.type != k_segment_load) continue;
        // The rest of the segment (mem_size - file_size, e.g. .bss) stays zero-filled.
        memory.write_bytes(segment.vaddr, file.subspan(segment.file_offset, segment.file_size));
    }
    for (const auto& [page, permissions] : page_permissions) {
        memory.protect(page, Memory::k_page_size, permissions);
    }

    // The entry point is a function descriptor: a 32-bit code address followed by a 32-bit
    // TOC pointer (the PS3 uses 32-bit pointers, unlike other 64-bit PowerPC systems).
    if (!memory.is_accessible(elf.entry, 8, Memory::k_read)) {
        throw LoadError(
            std::format("entry descriptor at {:#x} is not in a loaded segment", elf.entry));
    }
    return {
        .entry = memory.read<u32>(elf.entry),
        .toc = memory.read<u32>(elf.entry + 4),
    };
}

} // namespace loader
