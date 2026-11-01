#pragma once

// Builds big-endian ELF32 SPU executables in memory, so tests show exactly what they load.

#include "loader/elf.h"
#include "loader/spu_elf.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace test {

struct SpuSegmentSpec {
    u32 local_address;
    std::vector<u8> bytes;
    u32 memory_size; // at least bytes.size(); the rest is zero-filled
};

// A header, the program header table, then each segment's bytes in order.
inline std::vector<u8> make_spu_elf(const std::vector<SpuSegmentSpec>& segments, u32 entry = 0,
                                    u16 machine = loader::k_elf_machine_spu) {
    constexpr std::size_t header_size = 52;
    constexpr std::size_t program_header_size = 32;
    std::vector<u8> bytes(header_size + segments.size() * program_header_size);

    auto put = [&](std::size_t offset, u32 value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            bytes[offset + i] = static_cast<u8>(value >> (8 * (size - 1 - i)));
        }
    };
    const u8 identity[] = {0x7f, 'E', 'L', 'F', 1 /* 32-bit */, 2 /* big-endian */, 1};
    std::copy(std::begin(identity), std::end(identity), bytes.begin());
    put(16, loader::k_elf_type_exec, 2);
    put(18, machine, 2);
    put(24, entry, 4);
    put(28, header_size, 4); // program header table offset
    put(42, program_header_size, 2);
    put(44, static_cast<u32>(segments.size()), 2);

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const SpuSegmentSpec& s = segments[i];
        const std::size_t header = header_size + i * program_header_size;
        put(header, loader::k_segment_load, 4);
        put(header + 4, static_cast<u32>(bytes.size()), 4); // file offset
        put(header + 8, s.local_address, 4);
        put(header + 16, static_cast<u32>(s.bytes.size()), 4);
        put(header + 20, s.memory_size, 4);
        bytes.insert(bytes.end(), s.bytes.begin(), s.bytes.end());
    }
    return bytes;
}

} // namespace test
