#include "cpu/spu/interpreter.h"

#include "cpu/spu/disassembler.h"
#include "cpu/spu/opcodes.h"

#include <format>
#include <ostream>

namespace spu {

namespace {

void execute_one(Thread& t, std::ostream* trace) {
    const u32 raw = t.local_store.read<u32>(t.state.pc);
    if (trace) {
        *trace << std::format("spu {:#07x}  {:08x}  {}\n", t.state.pc, raw,
                              disassemble(raw, t.state.pc));
    }

    const OpcodeInfo* info = decoder().decode(raw);
    if (!info) {
        t.stop = Stop{StopReason::fault, std::format("illegal instruction {:#010x}", raw)};
        return;
    }

    t.next_pc = (t.state.pc + 4) & k_lslr;
    info->handler(t, Instruction(raw));

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
