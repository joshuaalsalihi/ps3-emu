// Branch, condition register, special register, trap and system instructions.
// Reference: PowerPC Architecture Book I (v2.02), chapter 2, "Branch Processor", and
// chapter 3, "Fixed-Point Processor".

#include "cpu/ppu/opcodes.h"

#include <format>

namespace ppu {

namespace {

void fault(Thread& t, std::string message) {
    t.stop = Stop{StopReason::fault, std::move(message)};
}

// ---- Branch ----

// Decodes the BO field (bits numbered 0-4 from the most significant bit) to decide whether a
// conditional branch is taken. Decrements CTR unless BO says to ignore it.
bool branch_taken(Thread& t, u32 bo, u32 bi) {
    const bool ignore_condition = bo & 0b10000;
    const bool branch_if_true = bo & 0b01000;
    const bool ignore_ctr = bo & 0b00100;
    const bool branch_if_ctr_zero = bo & 0b00010;

    if (!ignore_ctr) t.state.ctr -= 1;

    const bool ctr_ok = ignore_ctr || ((t.state.ctr == 0) == branch_if_ctr_zero);
    const bool condition_ok = ignore_condition || (t.state.cr_bit(bi) == branch_if_true);
    return ctr_ok && condition_ok;
}

// Saves the return address if LK is set. Call after reading LR for the branch target.
void link_if_requested(Thread& t, Instruction op) {
    if (op.lk()) t.state.lr = t.state.pc + 4;
}

void b(Thread& t, Instruction op) {
    const u64 target = op.aa() ? op.li() : t.state.pc + op.li();
    link_if_requested(t, op);
    t.next_pc = target;
}

void bc(Thread& t, Instruction op) {
    const bool taken = branch_taken(t, op.bo(), op.bi());
    link_if_requested(t, op);
    if (taken) t.next_pc = op.aa() ? op.bd() : t.state.pc + op.bd();
}

void bclr(Thread& t, Instruction op) {
    const u64 target = t.state.lr & ~u64{3};
    const bool taken = branch_taken(t, op.bo(), op.bi());
    link_if_requested(t, op);
    if (taken) t.next_pc = target;
}

void bcctr(Thread& t, Instruction op) {
    // bcctr cannot decrement CTR (the manuals call that form invalid), so ignore the CTR bit.
    const bool taken = branch_taken(t, op.bo() | 0b00100, op.bi());
    link_if_requested(t, op);
    if (taken) t.next_pc = t.state.ctr & ~u64{3};
}

// ---- Condition register logic ----

void crand(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), t.state.cr_bit(op.crba()) && t.state.cr_bit(op.crbb()));
}

void crandc(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), t.state.cr_bit(op.crba()) && !t.state.cr_bit(op.crbb()));
}

void creqv(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), t.state.cr_bit(op.crba()) == t.state.cr_bit(op.crbb()));
}

void crnand(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), !(t.state.cr_bit(op.crba()) && t.state.cr_bit(op.crbb())));
}

void crnor(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), !(t.state.cr_bit(op.crba()) || t.state.cr_bit(op.crbb())));
}

void cror(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), t.state.cr_bit(op.crba()) || t.state.cr_bit(op.crbb()));
}

void crorc(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), t.state.cr_bit(op.crba()) || !t.state.cr_bit(op.crbb()));
}

void crxor(Thread& t, Instruction op) {
    t.state.set_cr_bit(op.crbd(), t.state.cr_bit(op.crba()) != t.state.cr_bit(op.crbb()));
}

void mcrf(Thread& t, Instruction op) {
    t.state.set_cr_field(op.crfd(), t.state.cr_field(op.crfs()));
}

void mfcr(Thread& t, Instruction op) {
    // Also covers mfocrf, which may leave the other fields undefined; we return them all.
    t.gpr(op.rd()) = t.state.cr;
}

void mtcrf(Thread& t, Instruction op) {
    // Also covers mtocrf. Each CRM bit selects one 4-bit field, CRM's top bit being CR0.
    for (u32 field = 0; field < 8; ++field) {
        if (op.crm() & (0x80 >> field)) {
            t.state.set_cr_field(field, static_cast<u32>(t.gpr(op.rs()) >> (28 - 4 * field)));
        }
    }
}

// ---- Special registers ----

void mfspr(Thread& t, Instruction op) {
    u64& rd = t.gpr(op.rd());
    switch (op.spr()) {
    case k_spr_xer: rd = t.state.xer.value(); break;
    case k_spr_lr: rd = t.state.lr; break;
    case k_spr_ctr: rd = t.state.ctr; break;
    case k_spr_vrsave: rd = t.state.vrsave; break;
    case k_spr_tb: rd = t.clock.timebase(); break;
    case k_spr_tbu: rd = t.clock.timebase() >> 32; break;
    default: fault(t, std::format("mfspr from unsupported SPR {}", op.spr()));
    }
}

void mtspr(Thread& t, Instruction op) {
    const u64 value = t.gpr(op.rs());
    switch (op.spr()) {
    case k_spr_xer: t.state.xer.set(value); break;
    case k_spr_lr: t.state.lr = value; break;
    case k_spr_ctr: t.state.ctr = value; break;
    case k_spr_vrsave: t.state.vrsave = static_cast<u32>(value); break;
    default: fault(t, std::format("mtspr to unsupported SPR {}", op.spr()));
    }
}

void mftb(Thread& t, Instruction op) {
    // The TBR field is encoded like an SPR number.
    switch (op.spr()) {
    case k_spr_tb: t.gpr(op.rd()) = t.clock.timebase(); break;
    case k_spr_tbu: t.gpr(op.rd()) = t.clock.timebase() >> 32; break;
    default: fault(t, std::format("mftb from unsupported TBR {}", op.spr()));
    }
}

// ---- Traps ----

// The TO field selects which comparisons trigger the trap.
bool trap_condition(u32 to, s64 a, s64 b) {
    const u64 ua = static_cast<u64>(a);
    const u64 ub = static_cast<u64>(b);
    return ((to & 0b10000) && a < b) || ((to & 0b01000) && a > b) || ((to & 0b00100) && a == b) ||
           ((to & 0b00010) && ua < ub) || ((to & 0b00001) && ua > ub);
}

void trap_if(Thread& t, bool condition) {
    if (condition) t.stop = Stop{StopReason::trap, std::format("trap at {:#x}", t.state.pc)};
}

// Word traps compare the sign-extended low words.
s64 low_word(u64 value) {
    return static_cast<s32>(value);
}

void tw(Thread& t, Instruction op) {
    trap_if(t, trap_condition(op.to(), low_word(t.gpr(op.ra())), low_word(t.gpr(op.rb()))));
}

void twi(Thread& t, Instruction op) {
    trap_if(t, trap_condition(op.to(), low_word(t.gpr(op.ra())), op.simm()));
}

void td(Thread& t, Instruction op) {
    trap_if(t, trap_condition(op.to(), t.gpr(op.ra()), t.gpr(op.rb())));
}

void tdi(Thread& t, Instruction op) {
    trap_if(t, trap_condition(op.to(), t.gpr(op.ra()), op.simm()));
}

// ---- System ----

void sc(Thread& t, Instruction) {
    t.stop = Stop{StopReason::syscall, ""};
}

// Emulator-defined, not a PowerPC instruction: calls high-level-emulated library function number
// `index`. Imported library functions point at a thunk made of `hle` and `blr`. Primary opcode 1
// is unused by the PowerPC architecture, so real code never contains it.
void hle(Thread& t, Instruction) {
    t.stop = Stop{StopReason::hle_call, ""};
}

// Memory barriers. Guest threads run one at a time, so every access is already ordered.
void barrier(Thread&, Instruction) {}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic  form        op  xo   handler  operands               flags
    {"b",        Form::I,    18, 0,   b,       "LI",                  k_lk | k_aa},
    {"bc",       Form::B,    16, 0,   bc,      "BO, BI, BD",          k_lk | k_aa},
    {"bclr",     Form::XL,   19, 16,  bclr,    "BO, BI",              k_lk},
    {"bcctr",    Form::XL,   19, 528, bcctr,   "BO, BI",              k_lk},
    {"crand",    Form::XL,   19, 257, crand,   "crbD, crbA, crbB"},
    {"crandc",   Form::XL,   19, 129, crandc,  "crbD, crbA, crbB"},
    {"creqv",    Form::XL,   19, 289, creqv,   "crbD, crbA, crbB"},
    {"crnand",   Form::XL,   19, 225, crnand,  "crbD, crbA, crbB"},
    {"crnor",    Form::XL,   19, 33,  crnor,   "crbD, crbA, crbB"},
    {"cror",     Form::XL,   19, 449, cror,    "crbD, crbA, crbB"},
    {"crorc",    Form::XL,   19, 417, crorc,   "crbD, crbA, crbB"},
    {"crxor",    Form::XL,   19, 193, crxor,   "crbD, crbA, crbB"},
    {"mcrf",     Form::XL,   19, 0,   mcrf,    "crfD, crfS"},
    {"mfcr",     Form::X,    31, 19,  mfcr,    "rD"},
    {"mtcrf",    Form::XFX,  31, 144, mtcrf,   "CRM, rS"},
    {"mfspr",    Form::XFX,  31, 339, mfspr,   "rD, SPR"},
    {"mtspr",    Form::XFX,  31, 467, mtspr,   "SPR, rS"},
    {"mftb",     Form::XFX,  31, 371, mftb,    "rD, SPR"},
    {"tw",       Form::X,    31, 4,   tw,      "TO, rA, rB"},
    {"twi",      Form::D,    3,  0,   twi,     "TO, rA, SIMM"},
    {"td",       Form::X,    31, 68,  td,      "TO, rA, rB"},
    {"tdi",      Form::D,    2,  0,   tdi,     "TO, rA, SIMM"},
    {"sc",       Form::SC,   17, 1,   sc,      ""},
    {"sync",     Form::X,    31, 598, barrier, ""},
    {"eieio",    Form::X,    31, 854, barrier, ""},
    {"isync",    Form::XL,   19, 150, barrier, ""},
    {"hle",      Form::I,    1,  0,   hle,     "index"},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> branch_opcodes() {
    return k_opcodes;
}

} // namespace ppu
