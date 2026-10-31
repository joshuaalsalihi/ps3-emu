#pragma once

// Test fixture that runs PPU instructions, written as assembly, on a fresh machine.
//
// Expected values in the instruction tests are worked out by hand from the pseudocode in the
// PowerPC Architecture Books (v2.02), unless a comment says otherwise.

#include "core/clock.h"
#include "core/memory.h"
#include "cpu/ppu/assembler.h"
#include "cpu/ppu/interpreter.h"
#include "cpu/ppu/thread.h"

#include <initializer_list>
#include <string_view>
#include <vector>

namespace test {

class PpuTest {
public:
    static constexpr u64 k_code = 0x10000;
    static constexpr u64 k_data = 0x20000; // one readable and writable page

    PpuTest() {
        memory.map(k_code, Memory::k_page_size, Memory::k_read_write);
        memory.map(k_data, Memory::k_page_size, Memory::k_read_write);
    }

    // Assembles `program` at k_code and runs it until execution reaches the end of the program
    // or an instruction stops the thread (syscall, trap or fault).
    void run(std::initializer_list<std::string_view> program) {
        const std::vector<std::string_view> lines(program);
        const std::vector<u32> words = ppu::assemble(lines, k_code);
        for (std::size_t i = 0; i < words.size(); ++i) {
            memory.write<u32>(k_code + 4 * i, words[i]);
        }

        thread.state.pc = k_code;
        const u64 end = k_code + 4 * words.size();
        for (int step = 0; step < 10'000 && thread.state.pc != end; ++step) {
            ppu::interpret(thread, 1);
            if (thread.stop) break;
        }
    }

    u64& r(u32 n) { return thread.state.gpr[n]; }
    ppu::State& state() { return thread.state; }
    ppu::Xer& xer() { return thread.state.xer; }
    u32 cr_field(u32 n) const { return thread.state.cr_field(n); }
    bool stopped_with(ppu::StopReason reason) const {
        return thread.stop && thread.stop->reason == reason;
    }

    VirtualClock clock;
    Memory memory;
    ppu::Thread thread{1, memory, clock};
};

} // namespace test
