#pragma once

#include "common/bits.h"
#include "common/types.h"

namespace ppu {

// A 32-bit PPU instruction word. Field names and bit numbers follow the PowerPC manuals,
// where bit 0 is the most significant bit.
class Instruction {
public:
    constexpr explicit Instruction(u32 raw) : raw_(raw) {}

    constexpr u32 raw() const { return raw_; }

    // Bits `first` to `last` inclusive.
    constexpr u32 bits(u32 first, u32 last) const {
        const u32 width = last - first + 1;
        return (raw_ >> (31 - last)) & ((u32{1} << width) - 1);
    }
    constexpr bool bit(u32 n) const { return bits(n, n) != 0; }

    constexpr u32 opcode() const { return bits(0, 5); }

    // Registers
    constexpr u32 rd() const { return bits(6, 10); }
    constexpr u32 rs() const { return bits(6, 10); }
    constexpr u32 ra() const { return bits(11, 15); }
    constexpr u32 rb() const { return bits(16, 20); }

    // Immediates
    constexpr s64 simm() const { return sign_extend(bits(16, 31), 16); }
    constexpr u64 uimm() const { return bits(16, 31); }
    constexpr s64 d() const { return simm(); }
    constexpr s64 ds() const { return sign_extend(bits(16, 29) << 2, 16); }

    // Flags
    constexpr bool rc() const { return bit(31); } // record: update CR0 (or CR1)
    constexpr bool oe() const { return bit(21); } // overflow enable: update XER[OV, SO]
    constexpr bool lk() const { return bit(31); } // link: save return address in LR
    constexpr bool aa() const { return bit(30); } // absolute address

    // Branches
    constexpr u32 bo() const { return bits(6, 10); }
    constexpr u32 bi() const { return bits(11, 15); }
    constexpr s64 bd() const { return sign_extend(bits(16, 29) << 2, 16); }
    constexpr s64 li() const { return sign_extend(bits(6, 29) << 2, 26); }

    // Condition register
    constexpr u32 crfd() const { return bits(6, 8); }
    constexpr u32 crfs() const { return bits(11, 13); }
    constexpr bool l() const { return bit(10); } // compare: 0 = 32-bit, 1 = 64-bit
    constexpr u32 crbd() const { return bits(6, 10); }
    constexpr u32 crba() const { return bits(11, 15); }
    constexpr u32 crbb() const { return bits(16, 20); }
    constexpr u32 crm() const { return bits(12, 19); }

    // Rotates: 32-bit forms (M-form) and 64-bit forms (MD/MDS-form), whose 6-bit fields are
    // split, with the high bit stored separately.
    constexpr u32 sh() const { return bits(16, 20); }
    constexpr u32 mb() const { return bits(21, 25); }
    constexpr u32 me() const { return bits(26, 30); }
    constexpr u32 sh64() const { return (bits(30, 30) << 5) | bits(16, 20); }
    constexpr u32 mb64() const { return (bits(26, 26) << 5) | bits(21, 25); }
    constexpr u32 me64() const { return mb64(); }

    // Special registers: the 10-bit SPR number is stored with its two halves swapped.
    constexpr u32 spr() const { return (bits(16, 20) << 5) | bits(11, 15); }
    constexpr u32 to() const { return bits(6, 10); }

    // The emulator-defined `hle` instruction's function index.
    constexpr u32 hle_index() const { return bits(6, 31); }

private:
    u32 raw_;
};

} // namespace ppu
