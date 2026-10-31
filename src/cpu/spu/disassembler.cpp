#include "cpu/spu/disassembler.h"

#include "cpu/spu/opcodes.h"
#include "cpu/spu/operands.h"

#include <format>

namespace spu {

namespace {

std::string reg(u32 n) {
    return std::format("${}", n);
}

std::string address(s64 value) {
    return std::format("{:#x}", static_cast<u32>(value) & k_lslr);
}

std::string format_operand(Form form, Operand operand, Instruction op, u32 pc) {
    switch (operand) {
    case Operand::RT: return reg(form == Form::RRR ? op.rrr_rt() : op.rt());
    case Operand::RA: return reg(op.ra());
    case Operand::RB: return reg(op.rb());
    case Operand::RC: return reg(op.rc());
    case Operand::I7: return std::to_string(op.i7());
    case Operand::I10: return std::to_string(op.i10());
    case Operand::I16: return std::to_string(op.signed_i16());
    case Operand::U16: return std::format("{:#x}", op.i16());
    case Operand::U18: return std::format("{:#x}", op.i18());
    case Operand::d7_RA: return std::format("{}({})", op.i7(), reg(op.ra()));
    case Operand::d10_RA: return std::format("{}({})", op.i10() * 16, reg(op.ra()));
    case Operand::rel16: return address(s64{pc} + s64{op.signed_i16()} * 4);
    case Operand::abs16: return address(s64{op.signed_i16()} * 4);
    case Operand::brinst: {
        const s32 offset = form == Form::RR ? op.hbr_ro() : op.hbra_ro();
        return address(s64{pc} + s64{offset} * 4);
    }
    case Operand::scale155: return std::to_string(155 - static_cast<s32>(op.i8()));
    case Operand::scale173: return std::to_string(173 - static_cast<s32>(op.i8()));
    case Operand::CA: return std::format("$ch{}", op.channel());
    case Operand::SA: return std::format("$sp{}", op.spr());
    case Operand::stop_type: return std::format("{:#x}", op.stop_type());
    }
    return "?";
}

std::string mnemonic_with_suffixes(const OpcodeInfo& info, Instruction op) {
    std::string text(info.mnemonic);
    if ((info.flags & k_interrupt_bits) && op.d()) text += 'd';
    if ((info.flags & k_interrupt_bits) && op.e()) text += 'e';
    if ((info.flags & k_channel_sync) && op.bit(11)) text += 'c';
    return text;
}

} // namespace

std::string disassemble(u32 raw, u32 pc) {
    const Instruction op(raw);
    const OpcodeInfo* info = decoder().decode(raw);
    if (!info) return std::format(".long {:#010x}", raw);

    std::string text = mnemonic_with_suffixes(*info, op);
    const auto operands = parse_operand_layout(info->operands);
    for (std::size_t i = 0; i < operands.size(); ++i) {
        text += i == 0 ? " " : ", ";
        text += format_operand(info->form, operands[i], op, pc);
    }
    return text;
}

} // namespace spu
