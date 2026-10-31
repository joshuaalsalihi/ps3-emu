#pragma once

#include "common/types.h"
#include "cpu/ppu/instruction.h"
#include "cpu/ppu/thread.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace ppu {

using Handler = void (*)(Thread&, Instruction);

// Instruction formats. Each one puts its extended opcode in a different place.
enum class Form {
    I,   // b
    B,   // bc
    SC,  // sc
    D,   // immediate operand
    DS,  // 64-bit loads/stores; extended opcode in bits 30-31
    M,   // 32-bit rotates
    MD,  // 64-bit rotate by immediate; extended opcode in bits 27-29
    MDS, // 64-bit rotate by register; extended opcode in bits 27-30
    X,   // extended opcode in bits 21-30
    XL,  // like X, for branches and CR operations
    XFX, // like X, for special registers
    XO,  // arithmetic; extended opcode in bits 22-30, with OE in bit 21
    XS,  // sradi; extended opcode in bits 21-29
};

// Opcode flags. The first four are optional bits that add a suffix to the mnemonic.
constexpr u8 k_rc = 1;         // bit 31: record form, updates CR0 ("add.")
constexpr u8 k_oe = 2;         // bit 21: overflow form, updates XER[OV] ("addo")
constexpr u8 k_lk = 4;         // bit 31: link, saves the return address in LR ("bl")
constexpr u8 k_aa = 8;         // bit 30: absolute branch target ("ba")
constexpr u8 k_rc_always = 16; // bit 31 must be 1 ("stwcx.")

// One row of the opcode table: how to recognize, execute and print an instruction.
struct OpcodeInfo {
    std::string_view mnemonic;
    Form form;
    u32 primary;
    u32 extended; // ignored for forms without an extended opcode
    Handler handler;
    // Operand layout for the disassembler and assembler, e.g. "rD, rA, SIMM".
    std::string_view operands;
    u8 flags = 0; // combination of the k_* flags above

    // Bits that identify the instruction, and their expected values.
    u32 mask() const;
    u32 match() const;
};

// Opcode tables, defined next to their handlers.
std::span<const OpcodeInfo> integer_opcodes();
std::span<const OpcodeInfo> branch_opcodes();
std::span<const OpcodeInfo> load_store_opcodes();

// Finds the opcode table entry for an instruction word.
class Decoder {
public:
    Decoder();

    // Returns nullptr for illegal or unimplemented instructions.
    const OpcodeInfo* decode(u32 raw) const;
    // Every opcode table entry, for tools such as the assembler.
    const std::vector<const OpcodeInfo*>& all() const { return all_; }

private:
    std::vector<const OpcodeInfo*> all_;
    // Entries grouped by primary opcode, so decoding scans a short list.
    std::array<std::vector<const OpcodeInfo*>, 64> by_primary_;
};

const Decoder& decoder();

} // namespace ppu
