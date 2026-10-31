#pragma once

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "hle/linker.h"
#include "lv2/kernel.h"

#include <iosfwd>
#include <limits>
#include <span>
#include <string>

struct RunOptions {
    u64 max_instructions = std::numeric_limits<u64>::max();
    std::ostream* trace = nullptr; // if set, every instruction and call is printed here
};

struct RunResult {
    enum class Outcome { exited, faulted, deadlocked, instruction_limit };

    Outcome outcome = Outcome::instruction_limit;
    s32 exit_status = 0; // when exited
    std::string message; // when faulted or deadlocked
    u64 instructions = 0;
};

// The whole emulated machine. It owns all emulator state.
class System {
public:
    // UNVERIFIED: the main thread's defaults when the program has no process parameters.
    static constexpr s32 k_default_priority = 1000;
    static constexpr u64 k_default_stack_size = 0x4'0000; // 256 KB

    // `tty` receives the guest's console output.
    explicit System(std::ostream& tty);

    // Loads an ELF executable, links its imports and creates its main thread.
    // Throws loader::LoadError.
    void load(std::span<const u8> elf_file);

    // Runs until the program exits, faults, deadlocks or hits the instruction limit.
    RunResult run(const RunOptions& options = {});

    Memory& memory() { return memory_; }
    lv2::Kernel& kernel() { return kernel_; }
    lv2::PpuThread& main_thread() { return *kernel_.threads().front(); }
    const VirtualClock& clock() const { return clock_; }

private:
    // Handles whatever stopped `thread`. Returns false if the run must end with a fault.
    bool handle_stop(lv2::PpuThread& thread, std::ostream* trace);

    VirtualClock clock_;
    Memory memory_;
    lv2::Kernel kernel_;
    hle::Linker linker_;
};
