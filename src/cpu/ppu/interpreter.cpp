#include "cpu/ppu/interpreter.h"

#include "cpu/ppu/disassembler.h"
#include "cpu/ppu/opcodes.h"

#include <format>
#include <ostream>

namespace ppu {

namespace {

void execute_one(Thread& t, std::ostream* trace) {
    try {
        const u32 raw = t.memory.read<u32>(t.state.pc);
        if (trace) {
            *trace << std::format("{:#010x}  {:08x}  {}\n", t.state.pc, raw,
                                  disassemble(raw, t.state.pc));
        }

        const OpcodeInfo* info = decoder().decode(raw);
        if (!info) {
            t.stop = Stop{StopReason::fault, std::format("illegal instruction {:#010x}", raw)};
            return;
        }

        t.next_pc = t.state.pc + 4;
        info->handler(t, Instruction(raw));
    } catch (const MemoryFault& fault) {
        t.stop = Stop{StopReason::fault, fault.what()};
    }

    const bool resumes_after =
        !t.stop || t.stop->reason == StopReason::syscall || t.stop->reason == StopReason::hle_call;
    if (resumes_after) t.state.pc = t.next_pc;
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

} // namespace ppu
