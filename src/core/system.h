#pragma once

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "cpu/ppu/thread.h"
#include "lv2/kernel.h"

#include <iosfwd>
#include <limits>
#include <span>
#include <string>

struct RunOptions {
    u64 max_instructions = std::numeric_limits<u64>::max();
    std::ostream* trace = nullptr; // if set, every instruction is printed here
};

struct RunResult {
    enum class Outcome { exited, faulted, instruction_limit };

    Outcome outcome = Outcome::instruction_limit;
    s32 exit_status = 0; // when exited
    std::string message; // when faulted
    u64 instructions = 0;
};

// The whole emulated machine. It owns all emulator state.
class System {
public:
    // Where the main thread's stack lives. UNVERIFIED: chosen to match the PS3's stack region as
    // described on psdevwiki; the size will come from the ELF's process parameters later.
    static constexpr u64 k_stack_base = 0xd000'0000;
    static constexpr u64 k_stack_size = 0x4'0000; // 256 KB

    // `tty` receives the guest's console output.
    explicit System(std::ostream& tty);

    // Loads an ELF executable and prepares the main thread to run it. Throws loader::LoadError.
    void load(std::span<const u8> elf_file);

    // Runs until the program exits, faults or hits the instruction limit.
    RunResult run(const RunOptions& options = {});

    Memory& memory() { return memory_; }
    ppu::Thread& main_thread() { return main_thread_; }
    const VirtualClock& clock() const { return clock_; }

private:
    VirtualClock clock_;
    Memory memory_;
    lv2::Kernel kernel_;
    ppu::Thread main_thread_;
};
