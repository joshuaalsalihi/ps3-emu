#pragma once

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "gpu/reference_backend.h"
#include "hle/linker.h"
#include "lv2/kernel.h"
#include "rsx/rsx.h"

#include <functional>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>

struct RunOptions {
    u64 max_instructions = std::numeric_limits<u64>::max();
    std::ostream* trace = nullptr; // if set, every instruction and call is printed here
    bool instant_dma = false;      // SPU DMA completes the moment it is issued (for debugging)

    enum class Gpu { reference, null, metal }; // metal: macOS only

    Gpu gpu = Gpu::reference;
    std::function<void(const rsx::Frame&)> on_frame = {}; // called with each frame displayed

    // For debuggers: stop before a PPU thread executes an instruction at one of these
    // addresses, or once the PPU thread `step_thread` has executed one instruction.
    const std::set<u64>* breakpoints = nullptr;
    std::optional<u32> step_thread = std::nullopt;

    // For going back in time: stop just before the PPU thread `thread` executes its
    // instruction number `executed` (counting from 0), as PpuThread::executed counts them.
    struct ThreadPosition {
        u32 thread = 0;
        u64 executed = 0;
    };
    std::optional<ThreadPosition> stop_at = std::nullopt;
};

struct RunResult {
    enum class Outcome {
        exited,
        faulted,
        deadlocked,
        instruction_limit,
        breakpoint,
        stepped,
        reached, // the stop_at position
    };

    Outcome outcome = Outcome::instruction_limit;
    s32 exit_status = 0; // when exited
    std::string message; // when faulted or deadlocked
    u64 instructions = 0;
    u32 thread = 0; // the PPU thread that faulted, reached a breakpoint or stepped
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

    // Runs until the program exits, faults, deadlocks or hits the instruction limit. A run
    // that stops at the limit can be continued by running again; where it stopped makes no
    // difference to the guest.
    RunResult run(const RunOptions& options = {});

    // ---- Save states (system_state.cpp) ----

    // The whole machine's state.
    std::vector<u8> save_state();
    // Restores a state from save_state(), replacing whatever the System was running. Throws
    // state::Error if the state is damaged or from another version, leaving the System
    // unusable.
    void load_state(std::span<const u8> bytes);
    void serialize(state::Archive& archive);

    // Instructions executed since the program was loaded, by every PPU and SPU thread.
    u64 instructions() const { return instructions_; }

    Memory& memory() { return memory_; }
    lv2::Kernel& kernel() { return kernel_; }
    lv2::PpuThread& main_thread() { return *kernel_.threads().front(); }
    rsx::Rsx& rsx() { return rsx_; }
    const VirtualClock& clock() const { return clock_; }

private:
    // The thread whose turn it is, and how many instructions the turn has left. A run can stop
    // in the middle of a turn, and the next run finishes it.
    struct Turn {
        lv2::PpuThread* ppu = nullptr;
        lv2::SpuThread* spu = nullptr;
        u64 remaining = 0;
    };

    // Lets the devices and the clock catch up, then picks the next thread to run. Returns
    // false if no thread can run.
    bool begin_turn();
    // Handles whatever stopped `thread`. Returns false if the run must end with a fault.
    bool handle_stop(lv2::PpuThread& thread, std::ostream* trace);
    std::optional<std::string> select_gpu(RunOptions::Gpu gpu);
    // Gives an SPU thread instructions from its turn. Returns false if the run must end with a
    // fault; `ended` says whether its turn is over.
    bool run_spu_thread(lv2::SpuThread& thread, u64 budget, std::ostream* trace, RunResult& result,
                        bool& ended);

    VirtualClock clock_;
    Memory memory_;
    lv2::Kernel kernel_;
    hle::Linker linker_;
    rsx::Rsx rsx_;
    gpu::ReferenceBackend reference_backend_;
    gpu::NullBackend null_backend_;
    std::unique_ptr<rsx::Backend> metal_backend_; // created when first asked for

    std::optional<Turn> turn_;
    u64 instructions_ = 0;
};
