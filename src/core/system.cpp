#include "core/system.h"

#include "cpu/ppu/disassembler.h"
#include "cpu/ppu/interpreter.h"
#include "loader/elf.h"
#include "loader/program.h"

#include <format>

namespace {

constexpr u32 k_main_thread_id = 1;

// Room above the initial stack pointer for the ABI's back chain and saved link register.
constexpr u64 k_initial_frame_size = 128;

std::string describe_stop(const ppu::Thread& thread) {
    const u64 pc = thread.state.pc;
    std::string text = std::format("{} (pc {:#x}", thread.stop->message, pc);
    if (thread.memory.is_accessible(pc, 4, Memory::k_read)) {
        text += ": " + ppu::disassemble(thread.memory.read<u32>(pc), pc);
    }
    return text + ")";
}

} // namespace

System::System(std::ostream& tty) : kernel_(tty), main_thread_(k_main_thread_id, memory_, clock_) {}

void System::load(std::span<const u8> elf_file) {
    const loader::ElfFile elf = loader::parse_elf(elf_file);
    const loader::LoadedProgram program = loader::load_program(elf, elf_file, memory_);

    memory_.map(k_stack_base, k_stack_size, Memory::k_read_write);

    ppu::State& state = main_thread_.state;
    state.pc = program.entry;
    state.gpr[1] = k_stack_base + k_stack_size - k_initial_frame_size;
    state.gpr[2] = program.toc;
}

RunResult System::run(const RunOptions& options) {
    RunResult result;

    while (result.instructions < options.max_instructions) {
        const u64 budget = options.max_instructions - result.instructions;
        result.instructions += ppu::interpret(main_thread_, budget, options.trace);
        if (!main_thread_.stop) break; // budget used up

        if (main_thread_.stop->reason != ppu::StopReason::syscall) {
            result.outcome = RunResult::Outcome::faulted;
            result.message = describe_stop(main_thread_);
            break;
        }

        kernel_.handle_syscall(main_thread_);
        if (const auto status = kernel_.exit_status()) {
            result.outcome = RunResult::Outcome::exited;
            result.exit_status = *status;
            break;
        }
    }
    return result;
}
