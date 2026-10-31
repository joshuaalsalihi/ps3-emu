#include "core/system.h"

#include "cpu/ppu/disassembler.h"
#include "cpu/ppu/instruction.h"
#include "cpu/ppu/interpreter.h"
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

} // namespace

System::System(std::ostream& tty) : kernel_(memory_, clock_, tty) {}

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

        kernel_.wake_timed_out_threads();
        lv2::PpuThread* thread = kernel_.next_thread();
        if (!thread) {
            // Nothing can run: let time pass until a timeout, or report how the run ended.
            if (kernel_.skip_to_next_timeout()) continue;
            if (kernel_.all_threads_finished()) {
                // UNVERIFIED: the exit status when the process ends because its threads did.
                result.outcome = RunResult::Outcome::exited;
                return result;
            }
            result.outcome = RunResult::Outcome::deadlocked;
            result.message = "every thread is waiting:" + kernel_.describe_waits();
            return result;
        }

        if (options.trace && thread != previous) {
            *options.trace << std::format("--- thread {:#x} \"{}\"\n", thread->id, thread->name);
        }
        previous = thread;

        const u64 budget =
            std::min(lv2::Kernel::k_time_slice, options.max_instructions - result.instructions);
        result.instructions += ppu::interpret(thread->cpu, budget, options.trace);

        if (thread->cpu.stop && !handle_stop(*thread, options.trace)) {
            result.outcome = RunResult::Outcome::faulted;
            result.message = std::format("thread {:#x} \"{}\": {}", thread->id, thread->name,
                                         describe_stop(thread->cpu));
            return result;
        }
    }
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
