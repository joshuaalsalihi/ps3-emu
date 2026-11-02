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
#include <span>
#include <string>

struct RunOptions {
    u64 max_instructions = std::numeric_limits<u64>::max();
    std::ostream* trace = nullptr; // if set, every instruction and call is printed here
    bool instant_dma = false;      // SPU DMA completes the moment it is issued (for debugging)

    enum class Gpu { reference, null, metal }; // metal: macOS only

    Gpu gpu = Gpu::reference;
    std::function<void(const rsx::Frame&)> on_frame = {}; // called with each frame displayed
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
    rsx::Rsx& rsx() { return rsx_; }
    const VirtualClock& clock() const { return clock_; }

private:
    // Handles whatever stopped `thread`. Returns false if the run must end with a fault.
    bool handle_stop(lv2::PpuThread& thread, std::ostream* trace);
    std::optional<std::string> select_gpu(RunOptions::Gpu gpu);
    bool run_spu_thread(lv2::SpuThread& thread, u64 budget, std::ostream* trace, RunResult& result);

    VirtualClock clock_;
    Memory memory_;
    lv2::Kernel kernel_;
    hle::Linker linker_;
    rsx::Rsx rsx_;
    gpu::ReferenceBackend reference_backend_;
    gpu::NullBackend null_backend_;
    std::unique_ptr<rsx::Backend> metal_backend_; // created when first asked for
};
