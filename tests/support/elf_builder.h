#pragma once

// Builds big-endian ELF64 PPU executables in memory, so tests show exactly what they load.

#include "common/endian.h"
#include "loader/elf.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace test {

constexpr std::size_t k_elf_header_size = 64;
constexpr std::size_t k_program_header_size = 56;

inline void put_be(std::vector<u8>& bytes, std::size_t offset, u64 value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (8 * (size - 1 - i)));
    }
}

// A header, then the program header table, then `data_size` zero bytes for segment contents.
// Segment offsets are taken as given, so tests can describe malformed files too.
inline std::vector<u8> make_elf(const std::vector<loader::ElfSegment>& segments,
                                u64 data_size = 0x100, u64 entry = 0x10200) {
    std::vector<u8> bytes(k_elf_header_size + segments.size() * k_program_header_size + data_size);

    const u8 identity[] = {
        0x7f, 'E', 'L', 'F',
        2, // 64-bit
        2, // big-endian
        1, // ELF version
    };
    std::copy(std::begin(identity), std::end(identity), bytes.begin());
    put_be(bytes, 16, loader::k_elf_type_exec, 2);
    put_be(bytes, 18, loader::k_elf_machine_ppc64, 2);
    put_be(bytes, 24, entry, 8);
    put_be(bytes, 32, k_elf_header_size, 8); // program header table offset
    put_be(bytes, 54, k_program_header_size, 2);
    put_be(bytes, 56, segments.size(), 2);

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const loader::ElfSegment& s = segments[i];
        const std::size_t at = k_elf_header_size + i * k_program_header_size;
        put_be(bytes, at + 0, s.type, 4);
        put_be(bytes, at + 4, s.flags, 4);
        put_be(bytes, at + 8, s.file_offset, 8);
        put_be(bytes, at + 16, s.vaddr, 8);
        put_be(bytes, at + 32, s.file_size, 8);
        put_be(bytes, at + 40, s.mem_size, 8);
        put_be(bytes, at + 48, s.align, 8);
    }
    return bytes;
}

// A segment to place in a program: its address, permissions and contents.
struct SegmentSpec {
    u64 vaddr;
    u32 flags;
    std::vector<u8> contents;
    u64 mem_size = 0; // 0 means the size of `contents`
};

// Builds a loadable program from LOAD segments.
inline std::vector<u8> make_program(const std::vector<SegmentSpec>& specs, u64 entry) {
    std::vector<loader::ElfSegment> segments;
    u64 offset = k_elf_header_size + specs.size() * k_program_header_size;
    for (const SegmentSpec& spec : specs) {
        segments.push_back({
            .type = loader::k_segment_load,
            .flags = spec.flags,
            .file_offset = offset,
            .vaddr = spec.vaddr,
            .file_size = spec.contents.size(),
            .mem_size = std::max<u64>(spec.mem_size, spec.contents.size()),
            .align = 0x10000,
        });
        offset += spec.contents.size();
    }

    std::vector<u8> bytes = make_elf(
        segments, offset - k_elf_header_size - specs.size() * k_program_header_size, entry);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::copy(specs[i].contents.begin(), specs[i].contents.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(segments[i].file_offset));
    }
    return bytes;
}

// Big-endian bytes of instruction words or other 32-bit values.
inline std::vector<u8> words_to_bytes(const std::vector<u32>& words) {
    std::vector<u8> bytes(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        store_be(bytes.data() + 4 * i, words[i]);
    }
    return bytes;
}

} // namespace test
