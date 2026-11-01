// Compare, branch, halt and hint-for-branch instructions.
// Reference: SPU ISA v1.2, chapters 7 "Compare, Branch, and Halt Instructions" and 8
// "Hint-for-Branch Instructions".

#include "cpu/spu/ops.h"

namespace spu {

namespace {

using namespace ops;

// ---- Compares ----
// Each element becomes all ones (true) or all zeros (false). Immediates are sign-extended to
// the element size; byte forms use the low 8 bits of I10.

Vector immediate_words(Instruction op) {
    return splat_word(static_cast<u32>(op.i10()));
}
Vector immediate_halves(Instruction op) {
    return splat_half(static_cast<u16>(op.i10()));
}
Vector immediate_bytes(Instruction op) {
    return splat_byte(static_cast<u8>(op.i10()));
}

u8 equal_byte(u8 a, u8 b) {
    return mask8(a == b);
}
u16 equal_half(u16 a, u16 b) {
    return mask16(a == b);
}
u32 equal_word(u32 a, u32 b) {
    return mask32(a == b);
}
u8 greater_byte(u8 a, u8 b) {
    return mask8(static_cast<s8>(a) > static_cast<s8>(b));
}
u16 greater_half(u16 a, u16 b) {
    return mask16(static_cast<s16>(a) > static_cast<s16>(b));
}
u32 greater_word(u32 a, u32 b) {
    return mask32(static_cast<s32>(a) > static_cast<s32>(b));
}
u8 logically_greater_byte(u8 a, u8 b) {
    return mask8(a > b);
}
u16 logically_greater_half(u16 a, u16 b) {
    return mask16(a > b);
}
u32 logically_greater_word(u32 a, u32 b) {
    return mask32(a > b);
}

void ceqb(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), rb(t, op), equal_byte));
}
void ceqbi(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), immediate_bytes(op), equal_byte));
}
void ceqh(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), rb(t, op), equal_half));
}
void ceqhi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), equal_half));
}
void ceq(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), equal_word));
}
void ceqi(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), equal_word));
}
void cgtb(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), rb(t, op), greater_byte));
}
void cgtbi(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), immediate_bytes(op), greater_byte));
}
void cgth(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), rb(t, op), greater_half));
}
void cgthi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), greater_half));
}
void cgt(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), greater_word));
}
void cgti(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), greater_word));
}
void clgtb(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), rb(t, op), logically_greater_byte));
}
void clgtbi(Thread& t, Instruction op) {
    set_rt(t, op, each_byte(ra(t, op), immediate_bytes(op), logically_greater_byte));
}
void clgth(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), rb(t, op), logically_greater_half));
}
void clgthi(Thread& t, Instruction op) {
    set_rt(t, op, each_half(ra(t, op), immediate_halves(op), logically_greater_half));
}
void clgt(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), rb(t, op), logically_greater_word));
}
void clgti(Thread& t, Instruction op) {
    set_rt(t, op, each_word(ra(t, op), immediate_words(op), logically_greater_word));
}

// ---- Halts ----
// Compare preferred slots and stop the SPU if the condition holds, leaving pc on the halt.

void halt_if(Thread& t, bool condition) {
    if (condition) t.stop = Stop{StopReason::halt, "halt instruction"};
}

void heq(Thread& t, Instruction op) {
    halt_if(t, t.preferred(op.ra()) == t.preferred(op.rb()));
}
void heqi(Thread& t, Instruction op) {
    halt_if(t, t.preferred(op.ra()) == static_cast<u32>(op.i10()));
}
void hgt(Thread& t, Instruction op) {
    halt_if(t, static_cast<s32>(t.preferred(op.ra())) > static_cast<s32>(t.preferred(op.rb())));
}
void hgti(Thread& t, Instruction op) {
    halt_if(t, static_cast<s32>(t.preferred(op.ra())) > op.i10());
}
void hlgt(Thread& t, Instruction op) {
    halt_if(t, t.preferred(op.ra()) > t.preferred(op.rb()));
}
void hlgti(Thread& t, Instruction op) {
    halt_if(t, t.preferred(op.ra()) > static_cast<u32>(op.i10()));
}

// ---- Branches ----

u32 relative_target(const Thread& t, Instruction op) {
    return (t.state.pc + static_cast<u32>(op.signed_i16() * 4)) & k_lslr;
}
u32 absolute_target(Instruction op) {
    return static_cast<u32>(op.signed_i16() * 4) & k_lslr;
}
// Indirect targets ignore the low two bits.
u32 indirect_target(const Thread& t, Instruction op) {
    return t.preferred(op.ra()) & k_lslr & ~u32{3};
}

// The link register gets the return address in its preferred slot.
void set_link(Thread& t, Instruction op) {
    set_rt(t, op, preferred_word((t.state.pc + 4) & k_lslr));
}

// An indirect branch that is taken can also enable or disable interrupts at its target.
void branch_indirect(Thread& t, Instruction op, u32 target) {
    t.next_pc = target;
    if (op.d()) t.state.interrupts_enabled = false;
    if (op.e()) t.state.interrupts_enabled = true;
}

void br(Thread& t, Instruction op) {
    t.next_pc = relative_target(t, op);
}
void bra(Thread& t, Instruction op) {
    t.next_pc = absolute_target(op);
}
void brsl(Thread& t, Instruction op) {
    set_link(t, op);
    t.next_pc = relative_target(t, op);
}
void brasl(Thread& t, Instruction op) {
    set_link(t, op);
    t.next_pc = absolute_target(op);
}
void bi(Thread& t, Instruction op) {
    branch_indirect(t, op, indirect_target(t, op));
}
void iret(Thread& t, Instruction op) {
    branch_indirect(t, op, t.state.srr0 & k_lslr & ~u32{3});
}
// Branch only if an SPU event is pending; the link is set either way.
void bisled(Thread& t, Instruction op) {
    const u32 target = indirect_target(t, op);
    set_link(t, op);
    if (t.event_pending) branch_indirect(t, op, target);
}
void bisl(Thread& t, Instruction op) {
    const u32 target = indirect_target(t, op);
    set_link(t, op);
    branch_indirect(t, op, target);
}

// Conditional branches test the preferred slot of RT: its word, or its halfword (bytes 2-3).
bool word_is_zero(const Thread& t, Instruction op) {
    return t.preferred(op.rt()) == 0;
}
bool half_is_zero(const Thread& t, Instruction op) {
    return (t.preferred(op.rt()) & 0xffff) == 0;
}

void brnz(Thread& t, Instruction op) {
    if (!word_is_zero(t, op)) t.next_pc = relative_target(t, op);
}
void brz(Thread& t, Instruction op) {
    if (word_is_zero(t, op)) t.next_pc = relative_target(t, op);
}
void brhnz(Thread& t, Instruction op) {
    if (!half_is_zero(t, op)) t.next_pc = relative_target(t, op);
}
void brhz(Thread& t, Instruction op) {
    if (half_is_zero(t, op)) t.next_pc = relative_target(t, op);
}
void biz(Thread& t, Instruction op) {
    if (word_is_zero(t, op)) branch_indirect(t, op, indirect_target(t, op));
}
void binz(Thread& t, Instruction op) {
    if (!word_is_zero(t, op)) branch_indirect(t, op, indirect_target(t, op));
}
void bihz(Thread& t, Instruction op) {
    if (half_is_zero(t, op)) branch_indirect(t, op, indirect_target(t, op));
}
void bihnz(Thread& t, Instruction op) {
    if (!half_is_zero(t, op)) branch_indirect(t, op, indirect_target(t, op));
}

// ---- Hints ----
// Branch hints only help the hardware predict; they have no effect on results.

void hint(Thread&, Instruction) {}

constexpr OpcodeInfo k_opcodes[] = {
    {"heq", Form::RR, 0x3d8, heq, "RA, RB"},
    {"heqi", Form::RI10, 0x7f, heqi, "RA, I10"},
    {"hgt", Form::RR, 0x258, hgt, "RA, RB"},
    {"hgti", Form::RI10, 0x4f, hgti, "RA, I10"},
    {"hlgt", Form::RR, 0x2d8, hlgt, "RA, RB"},
    {"hlgti", Form::RI10, 0x5f, hlgti, "RA, I10"},
    {"ceqb", Form::RR, 0x3d0, ceqb, "RT, RA, RB"},
    {"ceqbi", Form::RI10, 0x7e, ceqbi, "RT, RA, I10"},
    {"ceqh", Form::RR, 0x3c8, ceqh, "RT, RA, RB"},
    {"ceqhi", Form::RI10, 0x7d, ceqhi, "RT, RA, I10"},
    {"ceq", Form::RR, 0x3c0, ceq, "RT, RA, RB"},
    {"ceqi", Form::RI10, 0x7c, ceqi, "RT, RA, I10"},
    {"cgtb", Form::RR, 0x250, cgtb, "RT, RA, RB"},
    {"cgtbi", Form::RI10, 0x4e, cgtbi, "RT, RA, I10"},
    {"cgth", Form::RR, 0x248, cgth, "RT, RA, RB"},
    {"cgthi", Form::RI10, 0x4d, cgthi, "RT, RA, I10"},
    {"cgt", Form::RR, 0x240, cgt, "RT, RA, RB"},
    {"cgti", Form::RI10, 0x4c, cgti, "RT, RA, I10"},
    {"clgtb", Form::RR, 0x2d0, clgtb, "RT, RA, RB"},
    {"clgtbi", Form::RI10, 0x5e, clgtbi, "RT, RA, I10"},
    {"clgth", Form::RR, 0x2c8, clgth, "RT, RA, RB"},
    {"clgthi", Form::RI10, 0x5d, clgthi, "RT, RA, I10"},
    {"clgt", Form::RR, 0x2c0, clgt, "RT, RA, RB"},
    {"clgti", Form::RI10, 0x5c, clgti, "RT, RA, I10"},
    {"br", Form::RI16, 0x64, br, "rel16"},
    {"bra", Form::RI16, 0x60, bra, "abs16"},
    {"brsl", Form::RI16, 0x66, brsl, "RT, rel16"},
    {"brasl", Form::RI16, 0x62, brasl, "RT, abs16"},
    {"bi", Form::RR, 0x1a8, bi, "RA", k_interrupt_bits},
    {"iret", Form::RR, 0x1aa, iret, "RA", k_interrupt_bits},
    {"bisled", Form::RR, 0x1ab, bisled, "RT, RA", k_interrupt_bits},
    {"bisl", Form::RR, 0x1a9, bisl, "RT, RA", k_interrupt_bits},
    {"brnz", Form::RI16, 0x42, brnz, "RT, rel16"},
    {"brz", Form::RI16, 0x40, brz, "RT, rel16"},
    {"brhnz", Form::RI16, 0x46, brhnz, "RT, rel16"},
    {"brhz", Form::RI16, 0x44, brhz, "RT, rel16"},
    {"biz", Form::RR, 0x128, biz, "RT, RA", k_interrupt_bits},
    {"binz", Form::RR, 0x129, binz, "RT, RA", k_interrupt_bits},
    {"bihz", Form::RR, 0x12a, bihz, "RT, RA", k_interrupt_bits},
    {"bihnz", Form::RR, 0x12b, bihnz, "RT, RA", k_interrupt_bits},
    {"hbrp", Form::RR, 0x1ac, hint, "", k_prefetch},
    {"hbr", Form::RR, 0x1ac, hint, "brinst, RA"},
    {"hbra", Form::RI18, 0x08, hint, "brinst, abs16"},
    {"hbrr", Form::RI18, 0x09, hint, "brinst, rel16"},
};

} // namespace

std::span<const OpcodeInfo> branch_opcodes() {
    return k_opcodes;
}

} // namespace spu
