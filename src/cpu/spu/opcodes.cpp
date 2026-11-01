#include "cpu/spu/opcodes.h"

#include <algorithm>
#include <bit>

namespace spu {

namespace {

constexpr u32 opcode_length(Form form) {
    switch (form) {
    case Form::RR:
    case Form::RI7: return 11;
    case Form::RRR: return 4;
    case Form::RI8: return 10;
    case Form::RI10: return 8;
    case Form::RI16: return 9;
    case Form::RI18: return 7;
    }
    return 11;
}

constexpr u32 k_bit11 = u32{1} << (31 - 11);

} // namespace

u32 OpcodeInfo::mask() const {
    u32 result = ~u32{0} << (32 - opcode_length(form));
    if (flags & k_prefetch) result |= k_bit11;
    return result;
}

u32 OpcodeInfo::match() const {
    u32 result = opcode << (32 - opcode_length(form));
    if (flags & k_prefetch) result |= k_bit11;
    return result;
}

Decoder::Decoder() {
    for (const auto table : {load_store_opcodes(), integer_opcodes(), shift_rotate_opcodes(),
                             branch_opcodes(), floating_point_opcodes(), control_opcodes()}) {
        for (const OpcodeInfo& info : table) {
            all_.push_back(&info);
            const u32 free_bits = 11 - opcode_length(info.form);
            const u32 first = info.opcode << free_bits;
            for (u32 prefix = first; prefix < first + (u32{1} << free_bits); ++prefix) {
                by_prefix_[prefix].push_back(&info);
            }
        }
    }
    // Try the most specific rows first, so hbrp is found before hbr.
    for (auto& bucket : by_prefix_) {
        std::stable_sort(bucket.begin(), bucket.end(), [](const auto* a, const auto* b) {
            return std::popcount(a->mask()) > std::popcount(b->mask());
        });
    }
}

const OpcodeInfo* Decoder::decode(u32 raw) const {
    for (const OpcodeInfo* info : by_prefix_[raw >> 21]) {
        if ((raw & info->mask()) == info->match()) return info;
    }
    return nullptr;
}

const Decoder& decoder() {
    static const Decoder instance;
    return instance;
}

} // namespace spu
