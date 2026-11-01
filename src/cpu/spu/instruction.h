#pragma once

#include "common/bits.h"
#include "common/types.h"

namespace spu {

// A 32-bit SPU instruction word. Field names and bit numbers follow the SPU ISA (v1.2,
// section 2.3 "Instruction Formats"), where bit 0 is the most significant bit.
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

    // Registers. The RRR form (four registers) moves RT to bits 4-10 to make room for RC.
    constexpr u32 rt() const { return bits(25, 31); }
    constexpr u32 ra() const { return bits(18, 24); }
    constexpr u32 rb() const { return bits(11, 17); }
    constexpr u32 rc() const { return bits(25, 31); }
    constexpr u32 rrr_rt() const { return bits(4, 10); }

    // Immediates, named after their width.
    constexpr s32 i7() const { return static_cast<s32>(sign_extend(bits(11, 17), 7)); }
    constexpr u32 i8() const { return bits(10, 17); }
    constexpr s32 i10() const { return static_cast<s32>(sign_extend(bits(8, 17), 10)); }
    constexpr u32 i16() const { return bits(9, 24); }
    constexpr s32 signed_i16() const { return static_cast<s32>(sign_extend(i16(), 16)); }
    constexpr u32 i18() const { return bits(7, 24); }

    // Channel and special-purpose register numbers sit in the RA field.
    constexpr u32 channel() const { return ra(); }
    constexpr u32 spr() const { return ra(); }
    // stop: the 14-bit signal type passed to the PPU.
    constexpr u32 stop_type() const { return bits(18, 31); }

    // Feature bits of indirect branches: disable or enable interrupts at the target.
    constexpr bool d() const { return bit(12); }
    constexpr bool e() const { return bit(13); }

    // Hint-for-branch: the word offset to the branch being hinted (ROH and ROL joined). hbr
    // keeps ROH in bits 16-17; hbra and hbrr keep it in bits 7-8.
    constexpr s32 hbr_ro() const {
        return static_cast<s32>(sign_extend((bits(16, 17) << 7) | bits(25, 31), 9));
    }
    constexpr s32 hbra_ro() const {
        return static_cast<s32>(sign_extend((bits(7, 8) << 7) | bits(25, 31), 9));
    }

private:
    u32 raw_;
};

} // namespace spu
