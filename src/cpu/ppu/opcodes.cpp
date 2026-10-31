#include "cpu/ppu/opcodes.h"

#include <optional>

namespace ppu {

namespace {

// Mask covering instruction bits `first` to `last` (bit 0 = most significant).
constexpr u32 field_mask(u32 first, u32 last) {
    const u32 width = last - first + 1;
    return ((u32{1} << width) - 1) << (31 - last);
}

struct ExtendedField {
    u32 first;
    u32 last;
};

constexpr std::optional<ExtendedField> extended_field(Form form) {
    switch (form) {
    case Form::SC: return ExtendedField{30, 30}; // always 1
    case Form::DS: return ExtendedField{30, 31};
    case Form::MD: return ExtendedField{27, 29};
    case Form::MDS: return ExtendedField{27, 30};
    case Form::X:
    case Form::XL:
    case Form::XFX: return ExtendedField{21, 30};
    case Form::XO: return ExtendedField{22, 30};
    case Form::XS: return ExtendedField{21, 29};
    case Form::A: return ExtendedField{26, 30};
    case Form::VA: return ExtendedField{26, 31};
    case Form::VX: return ExtendedField{21, 31};
    case Form::VC: return ExtendedField{22, 31};
    default: return std::nullopt;
    }
}

} // namespace

u32 OpcodeInfo::mask() const {
    u32 result = field_mask(0, 5);
    if (const auto field = extended_field(form)) {
        result |= field_mask(field->first, field->last);
    }
    if (flags & k_rc_always) result |= field_mask(31, 31);
    return result;
}

u32 OpcodeInfo::match() const {
    u32 result = primary << 26;
    if (const auto field = extended_field(form)) {
        result |= extended << (31 - field->last);
    }
    if (flags & k_rc_always) result |= 1;
    return result;
}

Decoder::Decoder() {
    for (const auto table :
         {integer_opcodes(), branch_opcodes(), load_store_opcodes(), floating_point_opcodes(),
          vector_integer_opcodes(), vector_float_opcodes(), vector_permute_opcodes()}) {
        for (const OpcodeInfo& info : table) {
            by_primary_[info.primary].push_back(&info);
            all_.push_back(&info);
        }
    }
}

const OpcodeInfo* Decoder::decode(u32 raw) const {
    for (const OpcodeInfo* info : by_primary_[raw >> 26]) {
        if ((raw & info->mask()) == info->match()) return info;
    }
    return nullptr;
}

const Decoder& decoder() {
    static const Decoder instance;
    return instance;
}

} // namespace ppu
