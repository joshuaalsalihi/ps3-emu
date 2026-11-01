#include "core/system.h"

#include "cpu/ppu/disassembler.h"
#include "cpu/ppu/instruction.h"
#include "cpu/ppu/interpreter.h"
#include "cpu/spu/disassembler.h"
#include "cpu/spu/spe.h"
#include "loader/elf.h"
#include "loader/process_info.h"
#include "loader/program.h"

#include <algorithm>
#include <format>
#include <ostream>

namespace {

std::string describe_stop(const ppu::Thread& cpu) {
    const u64 pc = cpu.state.pc;
    std::string text = std::format("{} (pc {:#x}", cpu.stop->message, pc);
    if (cpu.memory.is_accessible(pc, 4, Memory::k_read)) {
        text += ": " + ppu::disassemble(cpu.memory.read<u32>(pc), pc);
    }
    return text + ")";
}

std::string describe_unknown_spu_stop(u32 signal) {
    // The SPU C runtime's exit() (also reached by returning from main) is `stop 0x2000 |
    // status`, a convention of Linux's SPU runtime. LV2's meaning for it, if any, is unknown.
    if ((signal & ~0xffu) == 0x2000) {
        return std::format("stop {:#x}: exit({}) from the SPU C runtime, which LV2 doesn't "
                           "define; use spu_thread_exit instead",
                           signal, signal & 0xff);
    }
    return std::format("stop {:#x}, which LV2 gives no meaning", signal);
}

} // namespace

System::System(std::ostream& tty)
    : kernel_(memory_, clock_, tty), rsx_(memory_, clock_), reference_backend_(memory_) {
    kernel_.rsx = &rsx_;
}

void System::load(std::span<const u8> elf_file) {
    const loader::ElfFile elf = loader::parse_elf(elf_file);
    const loader::LoadedProgram program = loader::load_program(elf, elf_file, memory_);

    try {
        linker_.link(loader::read_imports(elf, memory_), memory_);
    } catch (const MemoryFault& fault) {
        throw loader::LoadError(std::string("bad import tables: ") + fault.what());
    }

    for (const loader::ElfSegment& segment : elf.segments) {
        if (segment.type == loader::k_segment_tls) {
            kernel_.set_tls_template(segment.vaddr, segment.file_size, segment.mem_size);
        }
    }

    s32 priority = k_default_priority;
    u64 stack_size = k_default_stack_size;
    if (const auto param = loader::read_process_param(elf, memory_)) {
        priority = param->primary_priority;
        // UNVERIFIED: small values may be size codes rather than byte counts; ignore them.
        if (param->primary_stack_size >= Memory::k_page_size) {
            stack_size = param->primary_stack_size;
        }
    }

    lv2::PpuThread* main =
        kernel_.create_thread("main", program.entry, program.toc, 0, priority, stack_size);
    if (!main) throw loader::LoadError("no room for the main thread's stack");
    // Returning from the entry point ends the process.
    main->cpu.state.lr = lv2::Kernel::k_process_exit_stub;
    kernel_.start_thread(*main);
}

RunResult System::run(const RunOptions& options) {
    RunResult result;
    const lv2::PpuThread* previous = nullptr;
    const lv2::SpuThread* previous_spu = nullptr;
    kernel_.instant_dma = options.instant_dma;
    rsx_.set_backend(options.gpu == RunOptions::Gpu::reference
                         ? static_cast<rsx::Backend*>(&reference_backend_)
                         : &null_backend_);
    rsx_.on_frame = options.on_frame;
    rsx_.trace = options.trace;

    while (true) {
        if (const auto status = kernel_.exit_status()) {
            result.outcome = RunResult::Outcome::exited;
            result.exit_status = *status;
            return result;
        }
        if (result.instructions >= options.max_instructions) {
            result.outcome = RunResult::Outcome::instruction_limit;
            return result;
        }

        kernel_.update_spu_threads();
        kernel_.wake_timed_out_threads();
        rsx_.run();
        if (const auto& error = rsx_.error()) {
            result.outcome = RunResult::Outcome::faulted;
            result.message = *error;
            return result;
        }
        const lv2::Runnable next = kernel_.next_thread();
        if (!next) {
            // Nothing can run: let the RSX work or time pass, or report how the run ended.
            if (rsx_.has_work()) continue;
            if (kernel_.skip_to_next_timeout(rsx_.next_deadline())) continue;
            if (kernel_.all_threads_finished()) {
                // UNVERIFIED: the exit status when the process ends because its threads did.
                result.outcome = RunResult::Outcome::exited;
                return result;
            }
            result.outcome = RunResult::Outcome::deadlocked;
            result.message = "every thread is waiting:" + kernel_.describe_waits();
            if (const std::string rsx_wait = rsx_.describe_wait(); !rsx_wait.empty()) {
                result.message += "\n  " + rsx_wait;
            }
            return result;
        }

        const u64 budget =
            std::min(lv2::Kernel::k_time_slice, options.max_instructions - result.instructions);

        if (next.spu) {
            if (options.trace && next.spu != previous_spu) {
                *options.trace << std::format("--- SPU thread {:#x} \"{}\"\n", next.spu->id,
                                              next.spu->name);
            }
            previous_spu = next.spu;
            previous = nullptr;
            if (!run_spu_thread(*next.spu, budget, options.trace, result)) return result;
            continue;
        }

        lv2::PpuThread* thread = next.ppu;
        if (options.trace && thread != previous) {
            *options.trace << std::format("--- thread {:#x} \"{}\"\n", thread->id, thread->name);
        }
        previous = thread;
        previous_spu = nullptr;

        result.instructions += ppu::interpret(thread->cpu, budget, options.trace);

        if (thread->cpu.stop && !handle_stop(*thread, options.trace)) {
            result.outcome = RunResult::Outcome::faulted;
            result.message = std::format("thread {:#x} \"{}\": {}", thread->id, thread->name,
                                         describe_stop(thread->cpu));
            return result;
        }
    }
}

// Gives an SPU thread its turn. Returns false if the run must end with a fault.
bool System::run_spu_thread(lv2::SpuThread& thread, u64 budget, std::ostream* trace,
                            RunResult& result) {
    const spu::SpeRun run = spu::run(*thread.spe, budget, trace);
    result.instructions += run.instructions;

    std::string fault;
    switch (run.exit) {
    case spu::SpeExit::budget: return true;
    case spu::SpeExit::stalled: kernel_.block_spu_thread(thread, run.channel); return true;
    case spu::SpeExit::interrupt: kernel_.handle_spu_interrupt(thread); return true;
    case spu::SpeExit::stopped:
        if (trace) *trace << std::format("    SPU stop {:#x}\n", run.signal);
        if (kernel_.handle_spu_stop(thread, run.signal)) return true;
        fault = describe_unknown_spu_stop(run.signal);
        break;
    case spu::SpeExit::fault: fault = run.message; break;
    }

    // Show the instruction responsible: a stop has already moved pc past itself.
    const spu::Thread& cpu = thread.spe->cpu;
    const bool after = run.exit == spu::SpeExit::stopped;
    const u32 pc = after ? (cpu.state.pc - 4) & spu::k_lslr : cpu.state.pc;
    const u32 raw = cpu.local_store.read<u32>(pc);
    result.outcome = RunResult::Outcome::faulted;
    result.message = std::format("SPU thread {:#x} \"{}\": {} (pc {:#x}: {})", thread.id,
                                 thread.name, fault, pc, spu::disassemble(raw, pc));
    return false;
}

bool System::handle_stop(lv2::PpuThread& thread, std::ostream* trace) {
    switch (thread.cpu.stop->reason) {
    case ppu::StopReason::syscall: {
        if (trace) {
            const u64 number = thread.cpu.state.gpr[11];
            const auto* info =
                number <= 0xffff ? kernel_.find_syscall(static_cast<u32>(number)) : nullptr;
            *trace << std::format("    syscall {} {}\n", number, info ? info->name : "?");
        }
        kernel_.handle_syscall(thread);
        return true;
    }
    case ppu::StopReason::hle_call: {
        const u32 raw = memory_.read<u32>(thread.cpu.state.pc - 4);
        const u32 index = ppu::Instruction(raw).hle_index();
        if (trace) *trace << std::format("    call {}\n", linker_.name(index));
        linker_.call(index, kernel_, thread);
        return true;
    }
    default: return false;
    }
}
