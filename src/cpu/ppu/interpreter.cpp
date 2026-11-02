#include "cpu/ppu/interpreter.h"

#include "cpu/ppu/disassembler.h"
#include "cpu/ppu/opcodes.h"

#include <format>
#include <optional>
#include <ostream>
#include <string>

namespace ppu {

namespace {

// The registers an instruction changed, as " -> r3=0x1 cr=0x20000000", for traces. Showing
// values makes two traces diverge where a result first differs, not only where the control
// flow does.
std::string describe_changes(const State& before, const State& after) {
    std::string text;
    auto add = [&](std::string item) { text += (text.empty() ? "  -> " : " ") + item; };
    for (u32 i = 0; i < 32; ++i) {
        if (after.gpr[i] != before.gpr[i]) add(std::format("r{}={:#x}", i, after.gpr[i]));
    }
    for (u32 i = 0; i < 32; ++i) {
        if (after.fpr[i] != before.fpr[i]) add(std::format("f{}={:#x}", i, after.fpr[i]));
    }
    for (u32 i = 0; i < 32; ++i) {
        if (after.vr[i].bytes != before.vr[i].bytes) {
            add(std::format("v{}={:016x}{:016x}", i, after.vr[i].high(), after.vr[i].low()));
        }
    }
    if (after.cr != before.cr) add(std::format("cr={:#010x}", after.cr));
    if (after.lr != before.lr) add(std::format("lr={:#x}", after.lr));
    if (after.ctr != before.ctr) add(std::format("ctr={:#x}", after.ctr));
    if (after.xer.value() != before.xer.value()) add(std::format("xer={:#x}", after.xer.value()));
    if (after.fpscr != before.fpscr) add(std::format("fpscr={:#010x}", after.fpscr));
    if (after.vscr != before.vscr) add(std::format("vscr={:#x}", after.vscr));
    if (after.vrsave != before.vrsave) add(std::format("vrsave={:#x}", after.vrsave));
    return text;
}

void execute_one(Thread& t, std::ostream* trace) {
    // Traced instructions are printed once they have run, with the registers they changed.
    std::string line;
    std::optional<State> before;
    try {
        const u32 raw = t.memory.read<u32>(t.state.pc);
        if (trace) {
            line =
                std::format("{:#010x}  {:08x}  {}", t.state.pc, raw, disassemble(raw, t.state.pc));
            before = t.state;
        }

        const OpcodeInfo* info = decoder().decode(raw);
        if (!info) {
            t.stop = Stop{StopReason::fault, std::format("illegal instruction {:#010x}", raw)};
        } else {
            t.next_pc = t.state.pc + 4;
            info->handler(t, Instruction(raw));
        }
    } catch (const MemoryFault& fault) {
        t.stop = Stop{StopReason::fault, fault.what()};
    }
    if (before) *trace << line << describe_changes(*before, t.state) << "\n";

    const bool resumes_after =
        !t.stop || t.stop->reason == StopReason::syscall || t.stop->reason == StopReason::hle_call;
    if (resumes_after) t.state.pc = t.next_pc;
}

} // namespace

u64 interpret(Thread& thread, u64 max_instructions, std::ostream* trace,
              const std::set<u64>* breakpoints) {
    thread.stop.reset();
    u64 executed = 0;
    while (executed < max_instructions && !thread.stop) {
        if (breakpoints && breakpoints->contains(thread.state.pc)) {
            thread.stop = Stop{StopReason::breakpoint, "breakpoint"};
            break;
        }
        execute_one(thread, trace);
        // Timing model T0: every instruction takes one cycle.
        thread.clock.advance(1);
        ++executed;
    }
    return executed;
}

} // namespace ppu
