#include "cpu/ppu/disassembler.h"

#include "cpu/ppu/opcodes.h"
#include "cpu/ppu/operands.h"

#include <format>
#include <optional>

namespace ppu {

namespace {

std::string reg(u32 n) {
    return std::format("r{}", n);
}

std::string spr_name(u32 spr) {
    switch (spr) {
    case k_spr_xer: return "xer";
    case k_spr_lr: return "lr";
    case k_spr_ctr: return "ctr";
    case k_spr_vrsave: return "vrsave";
    case k_spr_tb: return "tb";
    case k_spr_tbu: return "tbu";
    default: return std::to_string(spr);
    }
}

std::string branch_target(Instruction op, u64 pc, s64 offset) {
    return std::format("{:#x}", op.aa() ? static_cast<u64>(offset) : pc + offset);
}

std::string format_operand(Operand operand, Instruction op, u64 pc) {
    switch (operand) {
    case Operand::rD: return reg(op.rd());
    case Operand::rS: return reg(op.rs());
    case Operand::rA: return reg(op.ra());
    case Operand::rB: return reg(op.rb());
    case Operand::SIMM: return std::to_string(op.simm());
    case Operand::UIMM: return std::format("{:#x}", op.uimm());
    case Operand::d_rA: return std::format("{}({})", op.d(), reg(op.ra()));
    case Operand::ds_rA: return std::format("{}({})", op.ds(), reg(op.ra()));
    case Operand::crfD: return std::format("cr{}", op.crfd());
    case Operand::crfS: return std::format("cr{}", op.crfs());
    case Operand::L: return std::to_string(op.l());
    case Operand::crbD: return std::to_string(op.crbd());
    case Operand::crbA: return std::to_string(op.crba());
    case Operand::crbB: return std::to_string(op.crbb());
    case Operand::BO: return std::to_string(op.bo());
    case Operand::BI: return std::to_string(op.bi());
    case Operand::BD: return branch_target(op, pc, op.bd());
    case Operand::LI: return branch_target(op, pc, op.li());
    case Operand::SH: return std::to_string(op.sh());
    case Operand::MB: return std::to_string(op.mb());
    case Operand::ME: return std::to_string(op.me());
    case Operand::sh: return std::to_string(op.sh64());
    case Operand::mb: return std::to_string(op.mb64());
    case Operand::me: return std::to_string(op.me64());
    case Operand::TO: return std::to_string(op.to());
    case Operand::SPR: return spr_name(op.spr());
    case Operand::CRM: return std::format("{:#x}", op.crm());
    }
    return "?";
}

std::string mnemonic_with_suffixes(const OpcodeInfo& info, Instruction op) {
    std::string text(info.mnemonic);
    if ((info.flags & k_oe) && op.oe()) text += 'o';
    if ((info.flags & k_rc) && op.rc()) text += '.';
    if ((info.flags & k_lk) && op.lk()) text += 'l';
    if ((info.flags & k_aa) && op.aa()) text += 'a';
    return text;
}

// "cr7, " for compares and branches on fields other than CR0, which is the default.
std::string cr_field_prefix(u32 field) {
    return field == 0 ? "" : std::format("cr{}, ", field);
}

// Condition names for "branch if true" and "branch if false" on each bit of a CR field.
constexpr std::string_view k_true_conditions[] = {"lt", "gt", "eq", "so"};
constexpr std::string_view k_false_conditions[] = {"ge", "le", "ne", "ns"};

// The BO patterns used by extended branch mnemonics, ignoring the branch-prediction hint bits.
enum class BranchKind { always, if_true, if_false, ctr_nonzero, ctr_zero, other };

BranchKind branch_kind(u32 bo) {
    if ((bo & 0b10100) == 0b10100) return BranchKind::always;
    if ((bo & 0b11100) == 0b01100) return BranchKind::if_true;
    if ((bo & 0b11100) == 0b00100) return BranchKind::if_false;
    if ((bo & 0b10110) == 0b10000) return BranchKind::ctr_nonzero;
    if ((bo & 0b10110) == 0b10010) return BranchKind::ctr_zero;
    return BranchKind::other;
}

std::optional<std::string> conditional_branch(Instruction op, u64 pc) {
    const std::string link = op.lk() ? "l" : "";
    const std::string absolute = op.aa() ? "a" : "";
    const std::string target = branch_target(op, pc, op.bd());
    const u32 field = op.bi() / 4;
    const u32 bit = op.bi() % 4;

    switch (branch_kind(op.bo())) {
    case BranchKind::if_true:
        return std::format("b{}{}{} {}{}", k_true_conditions[bit], link, absolute,
                           cr_field_prefix(field), target);
    case BranchKind::if_false:
        return std::format("b{}{}{} {}{}", k_false_conditions[bit], link, absolute,
                           cr_field_prefix(field), target);
    case BranchKind::ctr_nonzero: return std::format("bdnz{}{} {}", link, absolute, target);
    case BranchKind::ctr_zero: return std::format("bdz{}{} {}", link, absolute, target);
    default: return std::nullopt;
    }
}

// Compares print as cmpw, cmpd, cmplwi, ...: "l" for logical, the size, then "i" for immediate.
std::string compare(std::string_view base, Instruction op, std::string_view immediate,
                    std::string_view second) {
    const char* size = op.l() ? "d" : "w";
    return std::format("{}{}{} {}{}, {}", base, size, immediate, cr_field_prefix(op.crfd()),
                       reg(op.ra()), second);
}

// Extended mnemonics: the common shorthand forms from the manuals' appendix.
std::optional<std::string> extended_mnemonic(const OpcodeInfo& info, Instruction op, u64 pc) {
    const std::string_view name = info.mnemonic;
    const std::string link = op.lk() ? "l" : "";

    if (name == "addi" && op.ra() == 0) return std::format("li {}, {}", reg(op.rd()), op.simm());
    if (name == "addis" && op.ra() == 0) return std::format("lis {}, {}", reg(op.rd()), op.simm());
    if (name == "ori" && op.raw() == 0x60000000) return "nop";
    if (name == "or" && op.rs() == op.rb()) {
        return std::format("mr{} {}, {}", op.rc() ? "." : "", reg(op.ra()), reg(op.rs()));
    }
    if (name == "cmp") return compare("cmp", op, "", reg(op.rb()));
    if (name == "cmpl") return compare("cmpl", op, "", reg(op.rb()));
    if (name == "cmpi") return compare("cmp", op, "i", std::to_string(op.simm()));
    if (name == "cmpli") return compare("cmpl", op, "i", std::format("{:#x}", op.uimm()));
    if (name == "bc") return conditional_branch(op, pc);
    if (name == "bclr" && branch_kind(op.bo()) == BranchKind::always) return "blr" + link;
    if (name == "bcctr" && branch_kind(op.bo()) == BranchKind::always) return "bctr" + link;
    if (name == "mfspr" && op.spr() == k_spr_lr) return "mflr " + reg(op.rd());
    if (name == "mfspr" && op.spr() == k_spr_ctr) return "mfctr " + reg(op.rd());
    if (name == "mtspr" && op.spr() == k_spr_lr) return "mtlr " + reg(op.rs());
    if (name == "mtspr" && op.spr() == k_spr_ctr) return "mtctr " + reg(op.rs());
    return std::nullopt;
}

} // namespace

std::string disassemble(u32 raw, u64 pc) {
    const Instruction op(raw);
    const OpcodeInfo* info = decoder().decode(raw);
    if (!info) return std::format(".long {:#010x}", raw);

    if (auto extended = extended_mnemonic(*info, op, pc)) return *extended;

    std::string text = mnemonic_with_suffixes(*info, op);
    const auto operands = parse_operand_layout(info->operands);
    for (std::size_t i = 0; i < operands.size(); ++i) {
        text += i == 0 ? " " : ", ";
        text += format_operand(operands[i], op, pc);
    }
    return text;
}

} // namespace ppu
