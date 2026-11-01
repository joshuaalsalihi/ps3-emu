#pragma once

// Test fixture for an SPE: an SPU with its DMA engine and channels, connected to a small guest
// memory. Programs are written as assembly; run() appends a `stop 0x3fff` so the end is clear.
//
// Expected values follow the Cell Broadband Engine Architecture (v1.01), unless a comment says
// otherwise.

#include "core/clock.h"
#include "core/memory.h"
#include "cpu/spu/assembler.h"
#include "cpu/spu/spe.h"

#include <initializer_list>
#include <string_view>
#include <vector>

namespace test {

class SpeTest {
public:
    static constexpr u64 k_main = 0x20000; // 64 KB of readable and writable guest memory
    static constexpr u32 k_end_signal = 0x3fff;

    SpeTest() { memory.map(k_main, 0x10000, Memory::k_read_write); }

    // Assembles `program` at address 0 and runs it from there.
    spu::SpeRun run(std::initializer_list<std::string_view> program, u64 budget = 10'000) {
        std::vector<std::string_view> lines(program);
        lines.push_back("stop 0x3fff");
        const std::vector<u32> words = spu::assemble(lines, 0);
        for (std::size_t i = 0; i < words.size(); ++i) {
            spe.cpu.local_store.write<u32>(4 * static_cast<u32>(i), words[i]);
        }
        spe.cpu.state.pc = 0;
        return resume(budget);
    }

    // Continues from where the SPU stopped.
    spu::SpeRun resume(u64 budget = 10'000) { return spu::run(spe, budget); }

    static bool finished(const spu::SpeRun& result) {
        return result.exit == spu::SpeExit::stopped && result.signal == k_end_signal;
    }
    static bool stalled_on(const spu::SpeRun& result, u32 channel) {
        return result.exit == spu::SpeExit::stalled && result.channel == channel;
    }

    Vector& r(u32 n) { return spe.cpu.state.gpr[n]; }
    // Puts `value` in the preferred slot of register n, with the rest zero.
    void set_value(u32 n, u32 value) { r(n) = Vector::from_words(value, 0, 0, 0); }
    u32 preferred(u32 n) const { return spe.cpu.preferred(n); }
    spu::LocalStore& local_store() { return spe.cpu.local_store; }

    VirtualClock clock;
    Memory memory;
    spu::Spe spe{0x5000, memory, clock};
};

} // namespace test
