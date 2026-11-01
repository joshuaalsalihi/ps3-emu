#pragma once

#include <string_view>
#include <vector>

namespace spu {

// Operand kinds named in the opcode table's operand layouts. Text forms follow spu-as.
// clang-format off
enum class Operand {
    RT, RA, RB, RC,      // registers, "$3" (RT moves to bits 4-10 in the RRR form)
    I7, I10, I16,        // signed immediates, printed in decimal
    U16, U18,            // unsigned immediates, printed in hexadecimal
    d7_RA,               // "offset(RA)" with a byte offset (cbd, chd, ...)
    d10_RA,              // "offset(RA)" with a quadword offset (lqd, stqd), printed in bytes
    rel16, abs16,        // a target address, relative to the instruction or absolute
    brinst,              // hint: the address of the branch being hinted, relative
    scale155, scale173,  // conversions: a scale encoded as 155 - I8 or 173 - I8
    CA, SA,              // channel "$ch29", special-purpose register "$sp0"
    stop_type,           // stop's 14-bit signal type
};
// clang-format on

// Parses an operand layout such as "RT, d10(RA)". Throws std::invalid_argument on unknown names.
std::vector<Operand> parse_operand_layout(std::string_view layout);

} // namespace spu
