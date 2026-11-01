// Load/store and constant-formation instructions.
// Reference: SPU ISA v1.2, chapters 3 "Memory—Load/Store Instructions" and 4 "Constant-Formation
// Instructions". Every load and store moves an aligned quadword; LocalStore drops the low four
// address bits and wraps the address around the local store.

#include "cpu/spu/ops.h"

namespace spu {

namespace {

using namespace ops;

// ---- Loads and stores ----

u32 d_form_address(const Thread& t, Instruction op) {
    return t.preferred(op.ra()) + static_cast<u32>(op.i10() * 16);
}
u32 x_form_address(const Thread& t, Instruction op) {
    return t.preferred(op.ra()) + t.preferred(op.rb());
}
u32 a_form_address(Instruction op) {
    return static_cast<u32>(op.signed_i16() * 4);
}
u32 relative_address(const Thread& t, Instruction op) {
    return t.state.pc + static_cast<u32>(op.signed_i16() * 4);
}

void load(Thread& t, Instruction op, u32 address) {
    set_rt(t, op, t.local_store.read_quad(address));
}
void store(Thread& t, Instruction op, u32 address) {
    t.local_store.write_quad(address, rt(t, op));
}

void lqd(Thread& t, Instruction op) {
    load(t, op, d_form_address(t, op));
}
void lqx(Thread& t, Instruction op) {
    load(t, op, x_form_address(t, op));
}
void lqa(Thread& t, Instruction op) {
    load(t, op, a_form_address(op));
}
void lqr(Thread& t, Instruction op) {
    load(t, op, relative_address(t, op));
}
void stqd(Thread& t, Instruction op) {
    store(t, op, d_form_address(t, op));
}
void stqx(Thread& t, Instruction op) {
    store(t, op, x_form_address(t, op));
}
void stqa(Thread& t, Instruction op) {
    store(t, op, a_form_address(op));
}
void stqr(Thread& t, Instruction op) {
    store(t, op, relative_address(t, op));
}

// ---- Generate controls for insertion ----
// These build a shufb pattern that keeps a quadword (bytes 0x10-0x1f select the second shufb
// operand) except for one element, which comes from the preferred slot of the first operand.
// Appendix B, "Details of the Generate Controls Instructions".

void insertion_controls(Thread& t, Instruction op, u32 address, u32 size) {
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        result.set_byte(i, static_cast<u8>(0x10 + i));
    }
    // The element goes where `address` points within its quadword. The source is the
    // preferred slot for its size: byte 3, bytes 2-3, bytes 0-3 or bytes 0-7.
    const u32 position = address & 0xf & ~(size - 1);
    const u32 source = size < 4 ? 4 - size : 0;
    for (u32 i = 0; i < size; ++i) {
        result.set_byte(position + i, static_cast<u8>(source + i));
    }
    set_rt(t, op, result);
}

u32 d7_address(const Thread& t, Instruction op) {
    return t.preferred(op.ra()) + static_cast<u32>(op.i7());
}

void cbd(Thread& t, Instruction op) {
    insertion_controls(t, op, d7_address(t, op), 1);
}
void cbx(Thread& t, Instruction op) {
    insertion_controls(t, op, x_form_address(t, op), 1);
}
void chd(Thread& t, Instruction op) {
    insertion_controls(t, op, d7_address(t, op), 2);
}
void chx(Thread& t, Instruction op) {
    insertion_controls(t, op, x_form_address(t, op), 2);
}
void cwd(Thread& t, Instruction op) {
    insertion_controls(t, op, d7_address(t, op), 4);
}
void cwx(Thread& t, Instruction op) {
    insertion_controls(t, op, x_form_address(t, op), 4);
}
void cdd(Thread& t, Instruction op) {
    insertion_controls(t, op, d7_address(t, op), 8);
}
void cdx(Thread& t, Instruction op) {
    insertion_controls(t, op, x_form_address(t, op), 8);
}

// ---- Constant formation ----

void ilh(Thread& t, Instruction op) {
    set_rt(t, op, splat_half(static_cast<u16>(op.i16())));
}
void ilhu(Thread& t, Instruction op) {
    set_rt(t, op, splat_word(op.i16() << 16));
}
void il(Thread& t, Instruction op) {
    set_rt(t, op, splat_word(static_cast<u32>(op.signed_i16())));
}
void ila(Thread& t, Instruction op) {
    set_rt(t, op, splat_word(op.i18()));
}
void iohl(Thread& t, Instruction op) {
    set_rt(t, op, each_word(rt(t, op), splat_word(op.i16()), [](u32 a, u32 b) { return a | b; }));
}

// Each bit of I16, from the most significant, selects 0xff or 0x00 for one byte.
void fsmbi(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        result.set_byte(i, mask8((op.i16() >> (15 - i)) & 1));
    }
    set_rt(t, op, result);
}

constexpr OpcodeInfo k_opcodes[] = {
    {"lqd", Form::RI10, 0x34, lqd, "RT, d10(RA)"},   {"lqx", Form::RR, 0x1c4, lqx, "RT, RA, RB"},
    {"lqa", Form::RI16, 0x61, lqa, "RT, abs16"},     {"lqr", Form::RI16, 0x67, lqr, "RT, rel16"},
    {"stqd", Form::RI10, 0x24, stqd, "RT, d10(RA)"}, {"stqx", Form::RR, 0x144, stqx, "RT, RA, RB"},
    {"stqa", Form::RI16, 0x41, stqa, "RT, abs16"},   {"stqr", Form::RI16, 0x47, stqr, "RT, rel16"},
    {"cbd", Form::RI7, 0x1f4, cbd, "RT, d7(RA)"},    {"cbx", Form::RR, 0x1d4, cbx, "RT, RA, RB"},
    {"chd", Form::RI7, 0x1f5, chd, "RT, d7(RA)"},    {"chx", Form::RR, 0x1d5, chx, "RT, RA, RB"},
    {"cwd", Form::RI7, 0x1f6, cwd, "RT, d7(RA)"},    {"cwx", Form::RR, 0x1d6, cwx, "RT, RA, RB"},
    {"cdd", Form::RI7, 0x1f7, cdd, "RT, d7(RA)"},    {"cdx", Form::RR, 0x1d7, cdx, "RT, RA, RB"},
    {"ilh", Form::RI16, 0x83, ilh, "RT, U16"},       {"ilhu", Form::RI16, 0x82, ilhu, "RT, U16"},
    {"il", Form::RI16, 0x81, il, "RT, I16"},         {"ila", Form::RI18, 0x21, ila, "RT, U18"},
    {"iohl", Form::RI16, 0xc1, iohl, "RT, U16"},     {"fsmbi", Form::RI16, 0x65, fsmbi, "RT, U16"},
};

} // namespace

std::span<const OpcodeInfo> load_store_opcodes() {
    return k_opcodes;
}

} // namespace spu
