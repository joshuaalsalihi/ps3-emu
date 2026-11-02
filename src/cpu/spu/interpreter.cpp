#include "cpu/spu/interpreter.h"

#include "cpu/spu/disassembler.h"
#include "cpu/spu/opcodes.h"

#include <format>
#include <optional>
#include <ostream>
#include <string>

namespace spu {

namespace {

// The registers an instruction changed, as " -> $3=0000000100000000...", for traces.
std::string describe_changes(const State& before, const State& after) {
    std::string text;
    auto add = [&](std::string item) { text += (text.empty() ? "  -> " : " ") + item; };
    for (u32 i = 0; i < before.gpr.size(); ++i) {
        if (after.gpr[i].bytes != before.gpr[i].bytes) {
            add(std::format("${}={:016x}{:016x}", i, after.gpr[i].high(), after.gpr[i].low()));
        }
    }
    if (after.fpscr.bytes != before.fpscr.bytes) {
        add(std::format("fpscr={:016x}{:016x}", after.fpscr.high(), after.fpscr.low()));
    }
    if (after.srr0 != before.srr0) add(std::format("srr0={:#x}", after.srr0));
    return text;
}

void execute_one(Thread& t, std::ostream* trace) {
    // Traced instructions are printed once they have run, with the registers they changed.
    const u32 raw = t.local_store.read<u32>(t.state.pc);
    std::optional<State> before;
    if (trace) before = t.state;

    const OpcodeInfo* info = decoder().decode(raw);
    if (!info) {
        t.stop = Stop{StopReason::fault, std::format("illegal instruction {:#010x}", raw)};
    } else {
        t.next_pc = (t.state.pc + 4) & k_lslr;
        info->handler(t, Instruction(raw));
    }
    if (before) {
        *trace << std::format("spu {:#07x}  {:08x}  {}", t.state.pc, raw,
                              disassemble(raw, t.state.pc))
               << describe_changes(*before, t.state) << "\n";
    }

    if (!t.stop || t.stop->reason == StopReason::stop) t.state.pc = t.next_pc;
}

} // namespace

u64 interpret(Thread& thread, u64 max_instructions, std::ostream* trace) {
    thread.stop.reset();
    u64 executed = 0;
    while (executed < max_instructions && !thread.stop) {
        execute_one(thread, trace);
        // Timing model T0: every instruction takes one cycle.
        thread.clock.advance(1);
        ++executed;
    }
    return executed;
}

} // namespace spu
