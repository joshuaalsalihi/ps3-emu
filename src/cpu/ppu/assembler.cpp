#include "cpu/ppu/assembler.h"

#include "cpu/ppu/opcodes.h"
#include "cpu/ppu/operands.h"

#include <charconv>
#include <format>
#include <optional>
#include <string>

namespace ppu {

namespace {

// ---- Text parsing ----

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
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

// Parses a register such as "r3" (prefix "r") or "cr7" (prefix "cr").
u32 parse_register(std::string_view text, std::string_view prefix, u32 count) {
    if (!text.starts_with(prefix)) {
        throw AssemblyError(std::format("expected a {} register, got '{}'", prefix, text));
    }
    const s64 number = parse_number(text.substr(prefix.size()));
    if (number < 0 || number >= count) {
        throw AssemblyError(std::format("no such register: '{}'", text));
    }
    return static_cast<u32>(number);
}

u32 parse_spr(std::string_view text) {
    if (text == "xer") return k_spr_xer;
    if (text == "lr") return k_spr_lr;
    if (text == "ctr") return k_spr_ctr;
    if (text == "vrsave") return k_spr_vrsave;
    if (text == "tb") return k_spr_tb;
    if (text == "tbu") return k_spr_tbu;
    return static_cast<u32>(parse_number(text));
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

// Inserts a 6-bit rotate field whose top bit is stored separately at `high_bit`.
void insert_split(u32& raw, u32 first, u32 high_bit, s64 value) {
    if (value < 0 || value > 63) throw AssemblyError(std::format("{} is not in 0..63", value));
    insert(raw, first, first + 4, value & 0x1f);
    insert(raw, high_bit, high_bit, value >> 5);
}

void insert_branch_offset(u32& raw, u32 first, u32 last, s64 target, u64 pc) {
    const bool absolute = Instruction(raw).aa();
    const s64 offset = absolute ? target : target - static_cast<s64>(pc);
    if (offset % 4 != 0) throw AssemblyError("branch target must be a multiple of 4");
    insert(raw, first, last, offset / 4, true);
}

// Parses "offset(rA)" into its two fields.
void insert_memory_operand(u32& raw, std::string_view text, bool ds_form) {
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos || !text.ends_with(')')) {
        throw AssemblyError(std::format("expected offset(rA), got '{}'", text));
    }
    const s64 offset = parse_number(text.substr(0, open));
    const u32 ra = parse_register(text.substr(open + 1, text.size() - open - 2), "r", 32);

    if (ds_form) {
        if (offset % 4 != 0) throw AssemblyError("ds offset must be a multiple of 4");
        insert(raw, 16, 29, offset / 4, true);
    } else {
        insert(raw, 16, 31, offset, true);
    }
    insert(raw, 11, 15, ra);
}

void insert_operand(u32& raw, Operand operand, std::string_view text, u64 pc) {
    switch (operand) {
    case Operand::rD:
    case Operand::rS: return insert(raw, 6, 10, parse_register(text, "r", 32));
    case Operand::rA: return insert(raw, 11, 15, parse_register(text, "r", 32));
    case Operand::rB: return insert(raw, 16, 20, parse_register(text, "r", 32));
    case Operand::SIMM: return insert(raw, 16, 31, parse_number(text), true);
    case Operand::UIMM: return insert(raw, 16, 31, parse_number(text));
    case Operand::d_rA: return insert_memory_operand(raw, text, false);
    case Operand::ds_rA: return insert_memory_operand(raw, text, true);
    case Operand::crfD: return insert(raw, 6, 8, parse_register(text, "cr", 8));
    case Operand::crfS: return insert(raw, 11, 13, parse_register(text, "cr", 8));
    case Operand::L: return insert(raw, 10, 10, parse_number(text));
    case Operand::crbD: return insert(raw, 6, 10, parse_number(text));
    case Operand::crbA: return insert(raw, 11, 15, parse_number(text));
    case Operand::crbB: return insert(raw, 16, 20, parse_number(text));
    case Operand::BO: return insert(raw, 6, 10, parse_number(text));
    case Operand::BI: return insert(raw, 11, 15, parse_number(text));
    case Operand::BD: return insert_branch_offset(raw, 16, 29, parse_number(text), pc);
    case Operand::LI: return insert_branch_offset(raw, 6, 29, parse_number(text), pc);
    case Operand::SH: return insert(raw, 16, 20, parse_number(text));
    case Operand::MB: return insert(raw, 21, 25, parse_number(text));
    case Operand::ME: return insert(raw, 26, 30, parse_number(text));
    case Operand::sh: return insert_split(raw, 16, 30, parse_number(text));
    case Operand::mb:
    case Operand::me: return insert_split(raw, 21, 26, parse_number(text));
    case Operand::TO: return insert(raw, 6, 10, parse_number(text));
    case Operand::SPR: {
        const u32 spr = parse_spr(text);
        insert(raw, 11, 15, spr & 0x1f);
        return insert(raw, 16, 20, spr >> 5);
    }
    case Operand::CRM: return insert(raw, 12, 19, parse_number(text));
    }
}

// ---- Mnemonics ----

// If `text` is `info`'s mnemonic plus valid optional suffixes, returns the flag bits those
// suffixes set. Suffix order matches the disassembler: "o" then ".", or "l" then "a".
std::optional<u32> match_mnemonic(const OpcodeInfo& info, std::string_view text) {
    if (!text.starts_with(info.mnemonic)) return std::nullopt;
    std::string_view rest = text.substr(info.mnemonic.size());
    u32 bits = 0;

    auto take = [&](u8 flag, char suffix, u32 bit) {
        if ((info.flags & flag) && rest.starts_with(suffix)) {
            bits |= u32{1} << (31 - bit);
            rest.remove_prefix(1);
        }
    };
    take(k_oe, 'o', 21);
    take(k_rc, '.', 31);
    take(k_lk, 'l', 31);
    take(k_aa, 'a', 30);

    if (!rest.empty()) return std::nullopt;
    return bits;
}

struct Statement {
    std::string mnemonic;
    std::vector<std::string> operands;
};

// "beq cr7, target" → "bc 12, 30, target", and so on.
std::optional<Statement> expand_conditional_branch(const Statement& s) {
    constexpr std::string_view true_conditions[] = {"lt", "gt", "eq", "so"};
    constexpr std::string_view false_conditions[] = {"ge", "le", "ne", "ns"};

    if (s.mnemonic.size() < 3 || s.mnemonic[0] != 'b') return std::nullopt;
    const std::string_view condition = std::string_view(s.mnemonic).substr(1, 2);
    const std::string suffixes = s.mnemonic.substr(3);

    for (u32 bit = 0; bit < 4; ++bit) {
        for (const bool if_true : {true, false}) {
            if (condition != (if_true ? true_conditions : false_conditions)[bit]) continue;

            std::vector<std::string> operands = s.operands;
            u32 field = 0;
            if (operands.size() == 2) {
                field = parse_register(operands[0], "cr", 8);
                operands.erase(operands.begin());
            }
            if (operands.size() != 1) throw AssemblyError("expected [crN,] target");
            return Statement{"bc" + suffixes,
                             {if_true ? "12" : "4", std::to_string(field * 4 + bit), operands[0]}};
        }
    }
    return std::nullopt;
}

// "cmpwi cr7, r3, 0" → "cmpi cr7, 0, r3, 0". The CR field defaults to cr0.
std::optional<Statement> expand_compare(const Statement& s) {
    constexpr std::string_view bases[] = {"cmp", "cmpl", "cmpi", "cmpli"};
    for (const std::string_view base : bases) {
        for (const char size : {'w', 'd'}) {
            std::string name(base);
            name.insert(base.starts_with("cmpl") ? 4 : 3, 1, size);
            if (s.mnemonic != name) continue;

            std::vector<std::string> operands = s.operands;
            if (operands.size() == 2) operands.insert(operands.begin(), "cr0");
            if (operands.size() != 3) throw AssemblyError("expected [crN,] rA, value");
            operands.insert(operands.begin() + 1, size == 'd' ? "1" : "0");
            return Statement{std::string(base), operands};
        }
    }
    return std::nullopt;
}

// Rewrites extended mnemonics (the shorthand the disassembler prints) into base instructions.
Statement expand(Statement s) {
    const auto& m = s.mnemonic;
    const auto& ops = s.operands;

    if (m == "nop") return {"ori", {"r0", "r0", "0"}};
    if (m == "li" && ops.size() == 2) return {"addi", {ops[0], "r0", ops[1]}};
    if (m == "lis" && ops.size() == 2) return {"addis", {ops[0], "r0", ops[1]}};
    if ((m == "mr" || m == "mr.") && ops.size() == 2) {
        return {m == "mr" ? "or" : "or.", {ops[0], ops[1], ops[1]}};
    }
    if (m == "blr" || m == "blrl") return {m == "blr" ? "bclr" : "bclrl", {"20", "0"}};
    if (m == "bctr" || m == "bctrl") return {m == "bctr" ? "bcctr" : "bcctrl", {"20", "0"}};
    if (m == "mflr" && ops.size() == 1) return {"mfspr", {ops[0], "lr"}};
    if (m == "mfctr" && ops.size() == 1) return {"mfspr", {ops[0], "ctr"}};
    if (m == "mtlr" && ops.size() == 1) return {"mtspr", {"lr", ops[0]}};
    if (m == "mtctr" && ops.size() == 1) return {"mtspr", {"ctr", ops[0]}};
    if (m.starts_with("bdnz") && ops.size() == 1) return {"bc" + m.substr(4), {"16", "0", ops[0]}};
    if (m.starts_with("bdz") && ops.size() == 1) return {"bc" + m.substr(3), {"18", "0", ops[0]}};
    if (auto compare = expand_compare(s)) return *compare;
    if (auto branch = expand_conditional_branch(s)) return *branch;
    return s;
}

} // namespace

u32 assemble(std::string_view line, u64 pc) {
    line = trim(line);
    const std::size_t space = line.find(' ');
    const Statement statement = expand({
        std::string(line.substr(0, space)),
        split_operands(space == std::string_view::npos ? "" : line.substr(space + 1)),
    });

    for (const OpcodeInfo* info : decoder().all()) {
        const auto flag_bits = match_mnemonic(*info, statement.mnemonic);
        if (!flag_bits) continue;

        const auto layout = parse_operand_layout(info->operands);
        if (layout.size() != statement.operands.size()) {
            throw AssemblyError(
                std::format("'{}' expects operands: {}", info->mnemonic, info->operands));
        }

        u32 raw = info->match() | *flag_bits;
        for (std::size_t i = 0; i < layout.size(); ++i) {
            insert_operand(raw, layout[i], statement.operands[i], pc);
        }
        return raw;
    }
    throw AssemblyError(std::format("unknown instruction '{}'", statement.mnemonic));
}

std::vector<u32> assemble(std::span<const std::string_view> lines, u64 base) {
    std::vector<u32> words;
    for (const std::string_view line : lines) {
        words.push_back(assemble(line, base + 4 * words.size()));
    }
    return words;
}

} // namespace ppu
