#pragma once

// Small whole programs for tests that run through System: PPU code at 0x10000, data at
// 0x20000, and optionally an SPU thread.

#include "cpu/ppu/assembler.h"
#include "cpu/spu/assembler.h"
#include "lv2/spu_thread.h"
#include "support/elf_builder.h"

#include <string_view>
#include <vector>

namespace test {

inline constexpr u64 k_code = 0x10000;
inline constexpr u64 k_data = 0x20000;
inline constexpr u64 k_data_size = 0x400;

inline constexpr u32 k_read_execute = loader::k_segment_flag_r | loader::k_segment_flag_x;
inline constexpr u32 k_read_write = loader::k_segment_flag_r | loader::k_segment_flag_w;

// A program with `code` at 0x10000 and `data` at 0x20000. The entry descriptor is the data's
// first 8 bytes, so data[0..7] must be {0x10000, 0}.
inline std::vector<u8> make_test_program(const std::vector<std::string_view>& code,
                                         std::vector<u8> data,
                                         const std::vector<InfoSegmentSpec>& info = {}) {
    return make_program(
        {
            {k_code, k_read_execute, words_to_bytes(ppu::assemble(code, k_code))},
            {k_data, k_read_write, std::move(data)},
        },
        k_data, info);
}

// Data with the entry descriptor followed by "hi\n" at 0x20008.
inline std::vector<u8> hello_data() {
    std::vector<u8> data = words_to_bytes({k_code, 0});
    data.insert(data.end(), {'h', 'i', '\n'});
    return data;
}

inline const std::vector<std::string_view> k_hello_and_exit = {
    "li r3, 0", "lis r4, 2", "addi r4, r4, 8",
    "li r5, 3", "li r6, 0",  "li r11, 403",
    "sc",                          // sys_tty_write
    "li r3, 7", "li r11, 3", "sc", // sys_process_exit
};

// Data for a program with one SPU thread. Layout, from 0x20000:
//   0x00 entry descriptor        0x10 group attributes (no name)   0x20 thread attributes
//   0x30 sys_spu_image_t         0x40 one copy segment             0x80 group ID, thread ID
//   0x88 join cause and status   0x90 a quadword the SPU writes    0x100 the SPU program
inline std::vector<u8> spu_program_data(const std::vector<std::string_view>& spu_code) {
    const std::vector<u32> spu_words = spu::assemble(spu_code, 0);
    std::vector<u32> words(0x100 / 4, 0);
    words[0] = static_cast<u32>(k_code);
    words[0x30 / 4 + 2] = static_cast<u32>(k_data + 0x40); // image: segments
    words[0x30 / 4 + 3] = 1;                               // image: segment count
    words[0x40 / 4] = lv2::k_spu_segment_copy;
    words[0x40 / 4 + 2] = static_cast<u32>(4 * spu_words.size());
    words[0x40 / 4 + 3] = static_cast<u32>(k_data + 0x100);
    words.insert(words.end(), spu_words.begin(), spu_words.end());
    return words_to_bytes(words);
}

// Creates a one-thread group, starts it, mails the SPU 42, joins the group, then exits the
// process with the first word the SPU wrote.
inline const std::vector<std::string_view> k_run_spu_thread = {
    "lis r9, 2",
    "addi r3, r9, 0x80",
    "li r4, 1",
    "li r5, 100",
    "addi r6, r9, 0x10",
    "li r11, 170",
    "sc",
    "addi r3, r9, 0x84",
    "lwz r4, 0x80(r9)",
    "li r5, 0",
    "addi r6, r9, 0x30",
    "addi r7, r9, 0x20",
    "li r8, 0",
    "li r11, 172",
    "sc",
    "lwz r3, 0x80(r9)",
    "li r11, 173",
    "sc",
    "lwz r3, 0x84(r9)",
    "li r4, 42",
    "li r11, 190",
    "sc",
    "lwz r3, 0x80(r9)",
    "addi r4, r9, 0x88",
    "addi r5, r9, 0x8c",
    "li r11, 178",
    "sc",
    "lwz r3, 0x90(r9)",
    "li r11, 3",
    "sc",
};

// Reads the mailbox, adds 1, puts the quadword to 0x20090 by DMA, waits for it, and exits.
inline const std::vector<std::string_view> k_spu_add_one = {
    "rdch $3, $ch29",  "ai $3, $3, 1",    "stqa $3, 0x200", "il $10, 0x200",   "ilhu $11, 0x2",
    "iohl $11, 0x90",  "il $12, 16",      "il $14, 0x20",   "wrch $ch16, $10", "wrch $ch18, $11",
    "wrch $ch19, $12", "wrch $ch21, $14", "il $16, 1",      "il $17, 2",       "wrch $ch22, $16",
    "wrch $ch23, $17", "rdch $5, $ch24",  "wrch $ch28, $3", "stop 0x102",
};

} // namespace test
