#include "cpu/spu/spe.h"

#include "cpu/spu/instruction.h"
#include "cpu/spu/interpreter.h"

#include <format>
#include <ostream>

namespace spu {

namespace {

// The channel instructions' opcodes (SPU ISA v1.2, chapter 11), as the top 11 bits.
constexpr u32 k_rdch = 0x00d;
constexpr u32 k_rchcnt = 0x00f;
constexpr u32 k_wrch = 0x10d;

std::string describe_channel(u32 number) {
    if (const auto name = channel_name(number)) return std::string(*name);
    return std::format("channel {}", number);
}

SpeRun fault(u64 instructions, std::string message) {
    return {SpeExit::fault, instructions, 0, 0, std::move(message)};
}

} // namespace

std::optional<u32> Spe::blocked_channel() {
    const Instruction op(cpu.local_store.read<u32>(cpu.state.pc));
    const u32 opcode = op.raw() >> 21;
    if (opcode != k_rdch && opcode != k_wrch) return std::nullopt;
    if (channels.count(op.channel()) != 0) return std::nullopt;
    return op.channel();
}

SpeRun run(Spe& spe, u64 budget, std::ostream* trace) {
    Thread& t = spe.cpu;
    u64 executed = 0;

    while (executed < budget) {
        spe.channels.update();
        if (const auto& error = spe.dma.error()) return fault(executed, *error);
        t.event_pending = spe.channels.event_waiting();

        executed += interpret(t, budget - executed, trace);
        if (!t.stop) continue;

        switch (t.stop->reason) {
        case StopReason::stop: return {SpeExit::stopped, executed, 0, t.stop->signal, ""};
        case StopReason::halt:
        case StopReason::fault: return fault(executed, t.stop->message);
        case StopReason::channel: break;
        }

        // A channel access: rdch, rchcnt or wrch. The interpreter left pc on it.
        const Instruction op(t.local_store.read<u32>(t.state.pc));
        const u32 opcode = op.raw() >> 21;
        const u32 number = op.channel();
        Channels& channels = spe.channels;

        if (opcode == k_rchcnt) {
            t.gpr(op.rt()) = Vector::from_words(channels.count(number), 0, 0, 0);
        } else if (opcode == k_rdch) {
            if (!channels.is_readable(number)) {
                return fault(executed, "rdch from " + describe_channel(number) + ", not readable");
            }
            if (channels.count(number) == 0) return {SpeExit::stalled, executed, number, 0, ""};
            const u32 value = channels.read(number);
            t.gpr(op.rt()) = Vector::from_words(value, 0, 0, 0);
            if (trace) *trace << std::format("    {} -> {:#x}\n", describe_channel(number), value);
        } else {
            if (!channels.is_writable(number)) {
                return fault(executed, "wrch to " + describe_channel(number) + ", not writable");
            }
            if (channels.count(number) == 0) return {SpeExit::stalled, executed, number, 0, ""};
            const u32 value = t.preferred(op.rt());
            if (trace) *trace << std::format("    {} <- {:#x}\n", describe_channel(number), value);
            channels.write(number, value);
        }
        t.state.pc = (t.state.pc + 4) & k_lslr;
    }

    if (const auto& error = spe.dma.error()) return fault(executed, *error);
    return {SpeExit::budget, executed, 0, 0, ""};
}

} // namespace spu
