#include "cpu/spu/assembler.h"

#include "cpu/spu/opcodes.h"
#include "cpu/spu/operands.h"

#include <charconv>
#include <format>
#include <optional>
#include <string>

namespace spu {

namespace {

// ---- Text parsing ----

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string> split_operands(std::string_view text) {
    std::vector<std::string> parts;
    while (!trim(text).empty()) {
        const std::size_t comma = text.find(',');
        parts.emplace_back(trim(text.substr(0, comma)));
        text = comma == std::string_view::npos ? "" : text.substr(comma + 1);
    }
    return parts;
}

// Parses a decimal or 0x-prefixed hexadecimal number, optionally negative.
s64 parse_number(std::string_view text) {
    const bool negative = text.starts_with('-');
    if (negative) text.remove_prefix(1);
    const bool hex = text.starts_with("0x");
    if (hex) text.remove_prefix(2);

    u64 value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, hex ? 16 : 10);
    if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
        throw AssemblyError(std::format("expected a number, got '{}'", text));
    }
    return negative ? -static_cast<s64>(value) : static_cast<s64>(value);
}

// Parses a numbered name such as "$3" (prefix "$") or "$ch29" (prefix "$ch").
u32 parse_numbered(std::string_view text, std::string_view prefix, u32 count) {
    if (!text.starts_with(prefix)) {
        throw AssemblyError(std::format("expected {}N, got '{}'", prefix, text));
    }
    const s64 number = parse_number(text.substr(prefix.size()));
    if (number < 0 || number >= count)
        throw AssemblyError(std::format("no such register: '{}'", text));
    return static_cast<u32>(number);
}

// General-purpose registers, including the ABI names compilers use for the first two.
u32 parse_register(std::string_view text) {
    if (text == "$lr") return 0;
    if (text == "$sp") return 1;
    return parse_numbered(text, "$", 128);
}

// ---- Encoding ----

// Stores `value` in instruction bits `first` to `last` (bit 0 = most significant), checking
// that it fits. Signed fields accept negative values.
void insert(u32& raw, u32 first, u32 last, s64 value, bool is_signed = false) {
    const u32 width = last - first + 1;
    const s64 min = is_signed ? -(s64{1} << (width - 1)) : 0;
    const s64 max = is_signed ? (s64{1} << (width - 1)) - 1 : (s64{1} << width) - 1;
    if (value < min || value > max) {
        throw AssemblyError(std::format("value {} does not fit in a {}-bit field", value, width));
    }
    const u32 mask = (u32{1} << width) - 1;
    raw |= (static_cast<u32>(value) & mask) << (31 - last);
}

s64 word_offset(s64 byte_offset) {
    if (byte_offset % 4 != 0) throw AssemblyError("branch target must be a multiple of 4");
    return byte_offset / 4;
}

// The word offset from `pc` to `target`. Addresses wrap around the local store, so the offset
// is taken modulo its size: 0x3fff8 is 8 bytes before address 0.
s64 relative_offset(s64 target, u32 pc) {
    const u32 distance = static_cast<u32>(target - pc) & k_lslr;
    return word_offset(sign_extend(distance, 18));
}

// Parses "offset(RA)". Quadword forms (lqd, stqd) store the offset divided by 16.
void insert_memory_operand(u32& raw, std::string_view text, bool quadword) {
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos || !text.ends_with(')')) {
        throw AssemblyError(std::format("expected offset(RA), got '{}'", text));
    }
    const s64 offset = parse_number(text.substr(0, open));
    insert(raw, 18, 24, parse_register(text.substr(open + 1, text.size() - open - 2)));

    if (!quadword) return insert(raw, 11, 17, offset, true);
    if (offset % 16 != 0) throw AssemblyError("quadword offset must be a multiple of 16");
    insert(raw, 8, 17, offset / 16, true);
}

// hbr keeps the high two bits of its 9-bit offset in bits 16-17; hbra and hbrr in bits 7-8.
void insert_branch_instruction(u32& raw, Form form, s64 words) {
    if (words < -256 || words > 255) throw AssemblyError("hinted branch is too far away");
    const u32 bits = static_cast<u32>(words) & 0x1ff;
    insert(raw, 25, 31, bits & 0x7f);
    if (form == Form::RR) {
        insert(raw, 16, 17, bits >> 7);
    } else {
        insert(raw, 7, 8, bits >> 7);
    }
}

void insert_operand(u32& raw, Form form, Operand operand, std::string_view text, u32 pc) {
    switch (operand) {
    case Operand::RT:
        if (form == Form::RRR) return insert(raw, 4, 10, parse_register(text));
        return insert(raw, 25, 31, parse_register(text));
    case Operand::RA: return insert(raw, 18, 24, parse_register(text));
    case Operand::RB: return insert(raw, 11, 17, parse_register(text));
    case Operand::RC: return insert(raw, 25, 31, parse_register(text));
    case Operand::I7: return insert(raw, 11, 17, parse_number(text), true);
    case Operand::I10: return insert(raw, 8, 17, parse_number(text), true);
    case Operand::I16: return insert(raw, 9, 24, parse_number(text), true);
    case Operand::U16: return insert(raw, 9, 24, parse_number(text));
    case Operand::U18: return insert(raw, 7, 24, parse_number(text));
    case Operand::d7_RA: return insert_memory_operand(raw, text, false);
    case Operand::d10_RA: return insert_memory_operand(raw, text, true);
    case Operand::rel16: return insert(raw, 9, 24, relative_offset(parse_number(text), pc), true);
    case Operand::abs16: return insert(raw, 9, 24, word_offset(parse_number(text)));
    case Operand::brinst:
        return insert_branch_instruction(raw, form, relative_offset(parse_number(text), pc));
    case Operand::scale155: return insert(raw, 10, 17, 155 - parse_number(text));
    case Operand::scale173: return insert(raw, 10, 17, 173 - parse_number(text));
    case Operand::CA: return insert(raw, 18, 24, parse_numbered(text, "$ch", 128));
    case Operand::SA: return insert(raw, 18, 24, parse_numbered(text, "$sp", 128));
    case Operand::stop_type: return insert(raw, 18, 31, parse_number(text));
    }
}

// ---- Mnemonics ----

// If `text` is `info`'s mnemonic plus valid optional suffixes, returns the feature bits those
// suffixes set: "d" or "e" on indirect branches ("bid"), "c" on sync ("syncc").
std::optional<u32> match_mnemonic(const OpcodeInfo& info, std::string_view text) {
    if (!text.starts_with(info.mnemonic)) return std::nullopt;
    const std::string_view rest = text.substr(info.mnemonic.size());
    if (rest.empty()) return 0;

    if (info.flags & k_interrupt_bits) {
        if (rest == "d") return u32{1} << (31 - 12);
        if (rest == "e") return u32{1} << (31 - 13);
    }
    if ((info.flags & k_channel_sync) && rest == "c") return u32{1} << (31 - 11);
    return std::nullopt;
}

} // namespace

u32 assemble(std::string_view line, u32 pc) {
    line = trim(line);
    const std::size_t space = line.find(' ');
    const std::string_view mnemonic = line.substr(0, space);
    const std::vector<std::string> operands =
        split_operands(space == std::string_view::npos ? "" : line.substr(space + 1));

    for (const OpcodeInfo* info : decoder().all()) {
        const auto feature_bits = match_mnemonic(*info, mnemonic);
        if (!feature_bits) continue;

        const auto layout = parse_operand_layout(info->operands);
        if (layout.size() != operands.size()) {
            throw AssemblyError(
                std::format("'{}' expects operands: {}", info->mnemonic, info->operands));
        }

        u32 raw = info->match() | *feature_bits;
        for (std::size_t i = 0; i < layout.size(); ++i) {
            insert_operand(raw, info->form, layout[i], operands[i], pc);
        }
        return raw;
    }
    throw AssemblyError(std::format("unknown instruction '{}'", mnemonic));
}

std::vector<u32> assemble(std::span<const std::string_view> lines, u32 base) {
    std::vector<u32> words;
    for (const std::string_view line : lines) {
        words.push_back(assemble(line, base + 4 * static_cast<u32>(words.size())));
    }
    return words;
}

} // namespace spu
