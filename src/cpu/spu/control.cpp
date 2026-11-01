// Control and channel instructions.
// Reference: SPU ISA v1.2, chapters 10 "Control Instructions" and 11 "Channel Instructions".

#include "cpu/spu/ops.h"

namespace spu {

namespace {

using namespace ops;

void stop(Thread& t, Instruction op) {
    t.stop = Stop{StopReason::stop, "stop", op.stop_type()};
}
void stopd(Thread& t, Instruction) {
    t.stop = Stop{StopReason::stop, "stopd", k_stopd_signal};
}

// The interpreter runs one instruction at a time, in order, so there is nothing to wait for.
void no_effect(Thread&, Instruction) {}

// UNVERIFIED: the Cell's SPU doesn't seem to define any special-purpose registers. Undefined
// ones read as zero and ignore writes.
void mfspr(Thread& t, Instruction op) {
    set_rt(t, op, Vector{});
}

// Channels connect the SPU to its MFC, mailboxes and signals. The interpreter leaves them to
// the code that owns those, stopping with pc still on the instruction.
void channel(Thread& t, Instruction) {
    t.stop = Stop{StopReason::channel, "channel access"};
}

constexpr OpcodeInfo k_opcodes[] = {
    {"stop", Form::RR, 0x000, stop, "stop_type"},
    {"stopd", Form::RR, 0x140, stopd, "RT, RA, RB"},
    {"lnop", Form::RR, 0x001, no_effect, ""},
    {"nop", Form::RR, 0x201, no_effect, "RT"},
    {"sync", Form::RR, 0x002, no_effect, "", k_channel_sync},
    {"dsync", Form::RR, 0x003, no_effect, ""},
    {"mfspr", Form::RR, 0x00c, mfspr, "RT, SA"},
    {"mtspr", Form::RR, 0x10c, no_effect, "SA, RT"},
    {"rdch", Form::RR, 0x00d, channel, "RT, CA"},
    {"rchcnt", Form::RR, 0x00f, channel, "RT, CA"},
    {"wrch", Form::RR, 0x10d, channel, "CA, RT"},
};

} // namespace

std::span<const OpcodeInfo> control_opcodes() {
    return k_opcodes;
}

} // namespace spu
