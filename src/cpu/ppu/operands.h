#pragma once

#include <string_view>
#include <vector>

namespace ppu {

// Operand kinds named in the opcode table's operand layouts. Names follow the manuals.
// Uppercase SH/MB/ME are the 5-bit fields of 32-bit rotates; lowercase sh/mb/me are the split
// 6-bit fields of 64-bit rotates.
// clang-format off
enum class Operand {
    rD, rS, rA, rB,          // general-purpose registers
    frD, frS, frA, frB, frC, // floating-point registers
    FLM, U,                  // mtfsf field mask, mtfsfi immediate
    vD, vS, vA, vB, vC,      // vector registers
    SHB, UIM, SIM, STRM,     // vsldoi shift, 5-bit immediates, data stream number
    SIMM, UIMM,              // 16-bit immediates
    d_rA, ds_rA,             // memory operand "offset(rA)"
    crfD, crfS, L,           // compare: CR field and operand size
    crbD, crbA, crbB,        // CR bit numbers
    BO, BI, BD, LI,          // branch fields; BD and LI are written as target addresses
    SH, MB, ME, sh, mb, me,  // rotate fields
    TO, SPR, CRM,            // trap condition, special register, CR field mask
    index,                   // the emulator-defined `hle` instruction's function index
};
// clang-format on

// Parses an operand layout such as "rD, d(rA)". Throws std::invalid_argument on unknown names.
std::vector<Operand> parse_operand_layout(std::string_view layout);

} // namespace ppu
