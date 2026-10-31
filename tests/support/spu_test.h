#pragma once

// Test fixture that runs SPU instructions, written as assembly, in a fresh local store.
//
// Expected values in the instruction tests are worked out by hand from the pseudocode in the
// SPU ISA (v1.2), unless a comment says otherwise.

#include "core/clock.h"
#include "cpu/spu/assembler.h"
#include "cpu/spu/interpreter.h"
#include "cpu/spu/thread.h"

#include <initializer_list>
#include <string_view>
#include <vector>

namespace test {

class SpuTest {
public:
    static constexpr u32 k_code = 0x0;
    static constexpr u32 k_data = 0x1000; // free local store for tests to use

    // Assembles `program` at k_code and runs it until execution reaches the end of the program
    // or an instruction stops the SPU.
    void run(std::initializer_list<std::string_view> program) {
        const std::vector<std::string_view> lines(program);
        const std::vector<u32> words = spu::assemble(lines, k_code);
        for (std::size_t i = 0; i < words.size(); ++i) {
            thread.local_store.write<u32>(k_code + 4 * static_cast<u32>(i), words[i]);
        }

        thread.state.pc = k_code;
        const u32 end = k_code + 4 * static_cast<u32>(words.size());
        for (int step = 0; step < 10'000 && thread.state.pc != end; ++step) {
            spu::interpret(thread, 1);
            if (thread.stop) break;
        }
    }

    Vector& r(u32 n) { return thread.state.gpr[n]; }
    void set_words(u32 n, u32 w0, u32 w1, u32 w2, u32 w3) {
        r(n) = Vector::from_words(w0, w1, w2, w3);
    }
    void set_all(u32 n, u32 word) { set_words(n, word, word, word, word); }
    u32 preferred(u32 n) const { return thread.preferred(n); }
    bool stopped_with(spu::StopReason reason) const {
        return thread.stop && thread.stop->reason == reason;
    }
    bool fpscr_bit(u32 bit) const { return thread.state.fpscr_bit(bit); }

    VirtualClock clock;
    spu::Thread thread{clock};
};

} // namespace test
