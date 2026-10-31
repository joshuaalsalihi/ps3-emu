#include "cpu/ppu/operands.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace ppu {

namespace {

constexpr std::pair<std::string_view, Operand> k_operand_names[] = {
    {"rD", Operand::rD},      {"rS", Operand::rS},        {"rA", Operand::rA},
    {"rB", Operand::rB},      {"SIMM", Operand::SIMM},    {"UIMM", Operand::UIMM},
    {"d(rA)", Operand::d_rA}, {"ds(rA)", Operand::ds_rA}, {"crfD", Operand::crfD},
    {"crfS", Operand::crfS},  {"L", Operand::L},          {"crbD", Operand::crbD},
    {"crbA", Operand::crbA},  {"crbB", Operand::crbB},    {"BO", Operand::BO},
    {"BI", Operand::BI},      {"BD", Operand::BD},        {"LI", Operand::LI},
    {"SH", Operand::SH},      {"MB", Operand::MB},        {"ME", Operand::ME},
    {"sh", Operand::sh},      {"mb", Operand::mb},        {"me", Operand::me},
    {"TO", Operand::TO},      {"SPR", Operand::SPR},      {"CRM", Operand::CRM},
};

Operand operand_from_name(std::string_view name) {
    for (const auto& [text, operand] : k_operand_names) {
        if (text == name) return operand;
    }
    throw std::invalid_argument("unknown operand name: " + std::string(name));
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

} // namespace

std::vector<Operand> parse_operand_layout(std::string_view layout) {
    std::vector<Operand> operands;
    while (!trim(layout).empty()) {
        const std::size_t comma = layout.find(',');
        operands.push_back(operand_from_name(trim(layout.substr(0, comma))));
        layout = comma == std::string_view::npos ? "" : layout.substr(comma + 1);
    }
    return operands;
}

} // namespace ppu
