// Load, store, atomic and cache instructions.
// Reference: PowerPC Architecture Book I (v2.02), chapter 3, "Fixed-Point Processor", and
// Book II, chapter 3, "Storage Control Instructions".
//
// Naming: "u" forms (update) write the effective address back to rA after the access;
// "x" forms (indexed) add rB instead of an immediate offset; "a" forms sign-extend.

#include "cpu/ppu/float_bits.h"
#include "cpu/ppu/opcodes.h"

#include <array>
#include <format>

namespace ppu {

namespace {

// Effective addresses for the non-update forms. (rA|0) means r0 reads as zero.
u64 ea_d(const Thread& t, Instruction op) {
    return t.gpr_or_zero(op.ra()) + op.d();
}

u64 ea_ds(const Thread& t, Instruction op) {
    return t.gpr_or_zero(op.ra()) + op.ds();
}

u64 ea_x(const Thread& t, Instruction op) {
    return t.gpr_or_zero(op.ra()) + t.state.gpr[op.rb()];
}

// Effective addresses for the update forms, which always use rA.
u64 ea_d_update(Thread& t, Instruction op) {
    return t.gpr(op.ra()) + op.d();
}

u64 ea_ds_update(Thread& t, Instruction op) {
    return t.gpr(op.ra()) + op.ds();
}

u64 ea_x_update(Thread& t, Instruction op) {
    return t.gpr(op.ra()) + t.gpr(op.rb());
}

// The low `T`-sized part of rS, for stores.
template <std::unsigned_integral T>
T low(const Thread& t, u32 reg) {
    return static_cast<T>(t.state.gpr[reg]);
}

// ---- Byte loads ----

void lbz(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u8>(ea_d(t, op));
}

void lbzx(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u8>(ea_x(t, op));
}

void lbzu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u8>(ea);
    t.gpr(op.ra()) = ea;
}

void lbzux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u8>(ea);
    t.gpr(op.ra()) = ea;
}

// ---- Halfword loads ----

void lhz(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u16>(ea_d(t, op));
}

void lhzx(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u16>(ea_x(t, op));
}

void lhzu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u16>(ea);
    t.gpr(op.ra()) = ea;
}

void lhzux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u16>(ea);
    t.gpr(op.ra()) = ea;
}

void lha(Thread& t, Instruction op) {
    t.gpr(op.rd()) = sign_extend(t.memory.read<u16>(ea_d(t, op)), 16);
}

void lhax(Thread& t, Instruction op) {
    t.gpr(op.rd()) = sign_extend(t.memory.read<u16>(ea_x(t, op)), 16);
}

void lhau(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.gpr(op.rd()) = sign_extend(t.memory.read<u16>(ea), 16);
    t.gpr(op.ra()) = ea;
}

void lhaux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.gpr(op.rd()) = sign_extend(t.memory.read<u16>(ea), 16);
    t.gpr(op.ra()) = ea;
}

void lhbrx(Thread& t, Instruction op) {
    t.gpr(op.rd()) = byte_swap(t.memory.read<u16>(ea_x(t, op)));
}

// ---- Word loads ----

void lwz(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u32>(ea_d(t, op));
}

void lwzx(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u32>(ea_x(t, op));
}

void lwzu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u32>(ea);
    t.gpr(op.ra()) = ea;
}

void lwzux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u32>(ea);
    t.gpr(op.ra()) = ea;
}

void lwa(Thread& t, Instruction op) {
    t.gpr(op.rd()) = sign_extend(t.memory.read<u32>(ea_ds(t, op)), 32);
}

void lwax(Thread& t, Instruction op) {
    t.gpr(op.rd()) = sign_extend(t.memory.read<u32>(ea_x(t, op)), 32);
}

void lwaux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.gpr(op.rd()) = sign_extend(t.memory.read<u32>(ea), 32);
    t.gpr(op.ra()) = ea;
}

void lwbrx(Thread& t, Instruction op) {
    t.gpr(op.rd()) = byte_swap(t.memory.read<u32>(ea_x(t, op)));
}

// ---- Doubleword loads ----

void ld(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u64>(ea_ds(t, op));
}

void ldx(Thread& t, Instruction op) {
    t.gpr(op.rd()) = t.memory.read<u64>(ea_x(t, op));
}

void ldu(Thread& t, Instruction op) {
    const u64 ea = ea_ds_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u64>(ea);
    t.gpr(op.ra()) = ea;
}

void ldux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.gpr(op.rd()) = t.memory.read<u64>(ea);
    t.gpr(op.ra()) = ea;
}

// ---- Stores ----

void stb(Thread& t, Instruction op) {
    t.memory.write(ea_d(t, op), low<u8>(t, op.rs()));
}

void stbx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), low<u8>(t, op.rs()));
}

void stbu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.memory.write(ea, low<u8>(t, op.rs()));
    t.gpr(op.ra()) = ea;
}

void stbux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.memory.write(ea, low<u8>(t, op.rs()));
    t.gpr(op.ra()) = ea;
}

void sth(Thread& t, Instruction op) {
    t.memory.write(ea_d(t, op), low<u16>(t, op.rs()));
}

void sthx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), low<u16>(t, op.rs()));
}

void sthu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.memory.write(ea, low<u16>(t, op.rs()));
    t.gpr(op.ra()) = ea;
}

void sthux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.memory.write(ea, low<u16>(t, op.rs()));
    t.gpr(op.ra()) = ea;
}

void sthbrx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), byte_swap(low<u16>(t, op.rs())));
}

void stw(Thread& t, Instruction op) {
    t.memory.write(ea_d(t, op), low<u32>(t, op.rs()));
}

void stwx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), low<u32>(t, op.rs()));
}

void stwu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.memory.write(ea, low<u32>(t, op.rs()));
    t.gpr(op.ra()) = ea;
}

void stwux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.memory.write(ea, low<u32>(t, op.rs()));
    t.gpr(op.ra()) = ea;
}

void stwbrx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), byte_swap(low<u32>(t, op.rs())));
}

void std_(Thread& t, Instruction op) {
    t.memory.write(ea_ds(t, op), t.gpr(op.rs()));
}

void stdx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), t.gpr(op.rs()));
}

void stdu(Thread& t, Instruction op) {
    const u64 ea = ea_ds_update(t, op);
    t.memory.write(ea, t.gpr(op.rs()));
    t.gpr(op.ra()) = ea;
}

void stdux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.memory.write(ea, t.gpr(op.rs()));
    t.gpr(op.ra()) = ea;
}

// ---- Floating-point doubleword ----
// These copy the 64-bit pattern unchanged, so they need no floating-point arithmetic.

void lfd(Thread& t, Instruction op) {
    t.state.fpr[op.rd()] = t.memory.read<u64>(ea_d(t, op));
}

void lfdx(Thread& t, Instruction op) {
    t.state.fpr[op.rd()] = t.memory.read<u64>(ea_x(t, op));
}

void lfdu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.state.fpr[op.rd()] = t.memory.read<u64>(ea);
    t.gpr(op.ra()) = ea;
}

void lfdux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.state.fpr[op.rd()] = t.memory.read<u64>(ea);
    t.gpr(op.ra()) = ea;
}

void stfd(Thread& t, Instruction op) {
    t.memory.write(ea_d(t, op), t.state.fpr[op.rs()]);
}

void stfdx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), t.state.fpr[op.rs()]);
}

void stfdu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.memory.write(ea, t.state.fpr[op.rs()]);
    t.gpr(op.ra()) = ea;
}

void stfdux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.memory.write(ea, t.state.fpr[op.rs()]);
    t.gpr(op.ra()) = ea;
}

// ---- Floating-point single ----
// Memory holds single precision; registers hold double. See float_bits.h.

void lfs(Thread& t, Instruction op) {
    t.state.fpr[op.rd()] = single_to_double_bits(t.memory.read<u32>(ea_d(t, op)));
}

void lfsx(Thread& t, Instruction op) {
    t.state.fpr[op.rd()] = single_to_double_bits(t.memory.read<u32>(ea_x(t, op)));
}

void lfsu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.state.fpr[op.rd()] = single_to_double_bits(t.memory.read<u32>(ea));
    t.gpr(op.ra()) = ea;
}

void lfsux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.state.fpr[op.rd()] = single_to_double_bits(t.memory.read<u32>(ea));
    t.gpr(op.ra()) = ea;
}

void stfs(Thread& t, Instruction op) {
    t.memory.write(ea_d(t, op), double_to_single_bits(t.state.fpr[op.rs()]));
}

void stfsx(Thread& t, Instruction op) {
    t.memory.write(ea_x(t, op), double_to_single_bits(t.state.fpr[op.rs()]));
}

void stfsu(Thread& t, Instruction op) {
    const u64 ea = ea_d_update(t, op);
    t.memory.write(ea, double_to_single_bits(t.state.fpr[op.rs()]));
    t.gpr(op.ra()) = ea;
}

void stfsux(Thread& t, Instruction op) {
    const u64 ea = ea_x_update(t, op);
    t.memory.write(ea, double_to_single_bits(t.state.fpr[op.rs()]));
    t.gpr(op.ra()) = ea;
}

// ---- Multiple word ----

void lmw(Thread& t, Instruction op) {
    u64 ea = ea_d(t, op);
    for (u32 reg = op.rd(); reg < 32; ++reg, ea += 4) {
        t.gpr(reg) = t.memory.read<u32>(ea);
    }
}

void stmw(Thread& t, Instruction op) {
    u64 ea = ea_d(t, op);
    for (u32 reg = op.rs(); reg < 32; ++reg, ea += 4) {
        t.memory.write(ea, low<u32>(t, reg));
    }
}

// ---- Atomics ----
// A load-and-reserve takes a reservation on the cache line; the matching store-conditional
// only stores if no one has written to that line since. CR0[EQ] reports success.

bool check_aligned(Thread& t, u64 ea, u64 alignment) {
    if (ea % alignment == 0) return true;
    t.stop = Stop{StopReason::fault, std::format("unaligned atomic access at {:#x}", ea)};
    return false;
}

void lwarx(Thread& t, Instruction op) {
    const u64 ea = ea_x(t, op);
    if (!check_aligned(t, ea, 4)) return;
    t.gpr(op.rd()) = t.memory.read<u32>(ea);
    t.memory.reserve(t.id, ea);
}

void ldarx(Thread& t, Instruction op) {
    const u64 ea = ea_x(t, op);
    if (!check_aligned(t, ea, 8)) return;
    t.gpr(op.rd()) = t.memory.read<u64>(ea);
    t.memory.reserve(t.id, ea);
}

void set_store_conditional_result(Thread& t, bool stored) {
    const u32 field = (stored ? k_cr_eq : 0) | (t.state.xer.so ? k_cr_so : 0);
    t.state.set_cr_field(0, field);
}

// UNVERIFIED: if the reservation is for a different line, the manuals let the store either
// happen or not. We don't store.
void stwcx(Thread& t, Instruction op) {
    const u64 ea = ea_x(t, op);
    if (!check_aligned(t, ea, 4)) return;
    const bool stored = t.memory.has_reservation(t.id, ea);
    if (stored) t.memory.write(ea, low<u32>(t, op.rs()));
    t.memory.clear_reservation(t.id);
    set_store_conditional_result(t, stored);
}

void stdcx(Thread& t, Instruction op) {
    const u64 ea = ea_x(t, op);
    if (!check_aligned(t, ea, 8)) return;
    const bool stored = t.memory.has_reservation(t.id, ea);
    if (stored) t.memory.write(ea, t.gpr(op.rs()));
    t.memory.clear_reservation(t.id);
    set_store_conditional_result(t, stored);
}

// ---- Cache ----

// Zeroes the whole cache line containing the address.
void dcbz(Thread& t, Instruction op) {
    const u64 line = ea_x(t, op) & ~(Memory::k_line_size - 1);
    constexpr std::array<u8, Memory::k_line_size> zeros{};
    t.memory.write_bytes(line, zeros);
}

// Cache hints and flushes. There is no cache to manage, so these do nothing.
void cache_hint(Thread&, Instruction) {}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic  form       op  xo    handler     operands
    {"lbz",      Form::D,   34, 0,    lbz,        "rD, d(rA)"},
    {"lbzu",     Form::D,   35, 0,    lbzu,       "rD, d(rA)"},
    {"lbzx",     Form::X,   31, 87,   lbzx,       "rD, rA, rB"},
    {"lbzux",    Form::X,   31, 119,  lbzux,      "rD, rA, rB"},
    {"lhz",      Form::D,   40, 0,    lhz,        "rD, d(rA)"},
    {"lhzu",     Form::D,   41, 0,    lhzu,       "rD, d(rA)"},
    {"lhzx",     Form::X,   31, 279,  lhzx,       "rD, rA, rB"},
    {"lhzux",    Form::X,   31, 311,  lhzux,      "rD, rA, rB"},
    {"lha",      Form::D,   42, 0,    lha,        "rD, d(rA)"},
    {"lhau",     Form::D,   43, 0,    lhau,       "rD, d(rA)"},
    {"lhax",     Form::X,   31, 343,  lhax,       "rD, rA, rB"},
    {"lhaux",    Form::X,   31, 375,  lhaux,      "rD, rA, rB"},
    {"lhbrx",    Form::X,   31, 790,  lhbrx,      "rD, rA, rB"},
    {"lwz",      Form::D,   32, 0,    lwz,        "rD, d(rA)"},
    {"lwzu",     Form::D,   33, 0,    lwzu,       "rD, d(rA)"},
    {"lwzx",     Form::X,   31, 23,   lwzx,       "rD, rA, rB"},
    {"lwzux",    Form::X,   31, 55,   lwzux,      "rD, rA, rB"},
    {"lwa",      Form::DS,  58, 2,    lwa,        "rD, ds(rA)"},
    {"lwax",     Form::X,   31, 341,  lwax,       "rD, rA, rB"},
    {"lwaux",    Form::X,   31, 373,  lwaux,      "rD, rA, rB"},
    {"lwbrx",    Form::X,   31, 534,  lwbrx,      "rD, rA, rB"},
    {"ld",       Form::DS,  58, 0,    ld,         "rD, ds(rA)"},
    {"ldu",      Form::DS,  58, 1,    ldu,        "rD, ds(rA)"},
    {"ldx",      Form::X,   31, 21,   ldx,        "rD, rA, rB"},
    {"ldux",     Form::X,   31, 53,   ldux,       "rD, rA, rB"},
    {"stb",      Form::D,   38, 0,    stb,        "rS, d(rA)"},
    {"stbu",     Form::D,   39, 0,    stbu,       "rS, d(rA)"},
    {"stbx",     Form::X,   31, 215,  stbx,       "rS, rA, rB"},
    {"stbux",    Form::X,   31, 247,  stbux,      "rS, rA, rB"},
    {"sth",      Form::D,   44, 0,    sth,        "rS, d(rA)"},
    {"sthu",     Form::D,   45, 0,    sthu,       "rS, d(rA)"},
    {"sthx",     Form::X,   31, 407,  sthx,       "rS, rA, rB"},
    {"sthux",    Form::X,   31, 439,  sthux,      "rS, rA, rB"},
    {"sthbrx",   Form::X,   31, 918,  sthbrx,     "rS, rA, rB"},
    {"stw",      Form::D,   36, 0,    stw,        "rS, d(rA)"},
    {"stwu",     Form::D,   37, 0,    stwu,       "rS, d(rA)"},
    {"stwx",     Form::X,   31, 151,  stwx,       "rS, rA, rB"},
    {"stwux",    Form::X,   31, 183,  stwux,      "rS, rA, rB"},
    {"stwbrx",   Form::X,   31, 662,  stwbrx,     "rS, rA, rB"},
    {"std",      Form::DS,  62, 0,    std_,       "rS, ds(rA)"},
    {"stdu",     Form::DS,  62, 1,    stdu,       "rS, ds(rA)"},
    {"stdx",     Form::X,   31, 149,  stdx,       "rS, rA, rB"},
    {"stdux",    Form::X,   31, 181,  stdux,      "rS, rA, rB"},
    {"lfs",      Form::D,   48, 0,    lfs,        "frD, d(rA)"},
    {"lfsu",     Form::D,   49, 0,    lfsu,       "frD, d(rA)"},
    {"lfsx",     Form::X,   31, 535,  lfsx,       "frD, rA, rB"},
    {"lfsux",    Form::X,   31, 567,  lfsux,      "frD, rA, rB"},
    {"stfs",     Form::D,   52, 0,    stfs,       "frS, d(rA)"},
    {"stfsu",    Form::D,   53, 0,    stfsu,      "frS, d(rA)"},
    {"stfsx",    Form::X,   31, 663,  stfsx,      "frS, rA, rB"},
    {"stfsux",   Form::X,   31, 695,  stfsux,     "frS, rA, rB"},
    {"lfd",      Form::D,   50, 0,    lfd,        "frD, d(rA)"},
    {"lfdu",     Form::D,   51, 0,    lfdu,       "frD, d(rA)"},
    {"lfdx",     Form::X,   31, 599,  lfdx,       "frD, rA, rB"},
    {"lfdux",    Form::X,   31, 631,  lfdux,      "frD, rA, rB"},
    {"stfd",     Form::D,   54, 0,    stfd,       "frS, d(rA)"},
    {"stfdu",    Form::D,   55, 0,    stfdu,      "frS, d(rA)"},
    {"stfdx",    Form::X,   31, 727,  stfdx,      "frS, rA, rB"},
    {"stfdux",   Form::X,   31, 759,  stfdux,     "frS, rA, rB"},
    {"lmw",      Form::D,   46, 0,    lmw,        "rD, d(rA)"},
    {"stmw",     Form::D,   47, 0,    stmw,       "rS, d(rA)"},
    {"lwarx",    Form::X,   31, 20,   lwarx,      "rD, rA, rB"},
    {"ldarx",    Form::X,   31, 84,   ldarx,      "rD, rA, rB"},
    {"stwcx.",   Form::X,   31, 150,  stwcx,      "rS, rA, rB", k_rc_always},
    {"stdcx.",   Form::X,   31, 214,  stdcx,      "rS, rA, rB", k_rc_always},
    {"dcbz",     Form::X,   31, 1014, dcbz,       "rA, rB"},
    {"dcbt",     Form::X,   31, 278,  cache_hint, "rA, rB"},
    {"dcbtst",   Form::X,   31, 246,  cache_hint, "rA, rB"},
    {"dcbf",     Form::X,   31, 86,   cache_hint, "rA, rB"},
    {"dcbst",    Form::X,   31, 54,   cache_hint, "rA, rB"},
    {"icbi",     Form::X,   31, 982,  cache_hint, "rA, rB"},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> load_store_opcodes() {
    return k_opcodes;
}

} // namespace ppu
