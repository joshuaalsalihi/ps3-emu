#pragma once

#include "common/types.h"
#include "cpu/spu/instruction.h"
#include "cpu/spu/thread.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace spu {

using Handler = void (*)(Thread&, Instruction);

// Instruction formats (SPU ISA v1.2, section 2.3), named after their operands. The opcode is
// always at the top of the word; its length depends on the format.
enum class Form {
    RR,   // two or three registers; 11-bit opcode
    RRR,  // four registers; 4-bit opcode
    RI7,  // 7-bit immediate; 11-bit opcode
    RI8,  // 8-bit immediate; 10-bit opcode
    RI10, // 10-bit immediate; 8-bit opcode
    RI16, // 16-bit immediate; 9-bit opcode
    RI18, // 18-bit immediate; 7-bit opcode
};

// Opcode flags: optional feature bits that add a suffix to the mnemonic, as spu-as writes them.
constexpr u8 k_interrupt_bits = 1; // bit 12 "d" / bit 13 "e": disable or enable interrupts ("bid")
constexpr u8 k_channel_sync = 2;   // bit 11 "c": sync also synchronizes channels ("syncc")
constexpr u8 k_prefetch = 4;       // bit 11 must be 1: hbr's inline-prefetch form ("hbrp")

// One row of the opcode table: how to recognize, execute and print an instruction.
struct OpcodeInfo {
    std::string_view mnemonic;
    Form form;
    u32 opcode; // the opcode bits, as a number (its length comes from the form)
    Handler handler;
    // Operand layout for the disassembler and assembler, e.g. "RT, RA, I10".
    std::string_view operands;
    u8 flags = 0; // combination of the k_* flags above

    // Bits that identify the instruction, and their expected values.
    u32 mask() const;
    u32 match() const;
};

// Opcode tables, defined next to their handlers, one per chapter of the SPU ISA.
std::span<const OpcodeInfo> load_store_opcodes();     // memory, constant formation
std::span<const OpcodeInfo> integer_opcodes();        // integer and logical
std::span<const OpcodeInfo> shift_rotate_opcodes();   // shift and rotate
std::span<const OpcodeInfo> branch_opcodes();         // compare, branch, halt, hint
std::span<const OpcodeInfo> floating_point_opcodes(); // floating point
std::span<const OpcodeInfo> control_opcodes();        // control and channel

// Finds the opcode table entry for an instruction word.
class Decoder {
public:
    Decoder();

    // Returns nullptr for illegal instructions.
    const OpcodeInfo* decode(u32 raw) const;
    // Every opcode table entry, for tools such as the assembler.
    const std::vector<const OpcodeInfo*>& all() const { return all_; }

private:
    std::vector<const OpcodeInfo*> all_;
    // Entries grouped by the top 11 bits, the longest opcode length. Shorter opcodes appear
    // under every 11-bit prefix they cover.
    std::array<std::vector<const OpcodeInfo*>, 2048> by_prefix_;
};

const Decoder& decoder();

} // namespace spu
