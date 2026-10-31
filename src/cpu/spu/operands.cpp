#include "cpu/spu/operands.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace spu {

namespace {

constexpr std::pair<std::string_view, Operand> k_operand_names[] = {
    {"RT", Operand::RT},
    {"RA", Operand::RA},
    {"RB", Operand::RB},
    {"RC", Operand::RC},
    {"I7", Operand::I7},
    {"I10", Operand::I10},
    {"I16", Operand::I16},
    {"U16", Operand::U16},
    {"U18", Operand::U18},
    {"d7(RA)", Operand::d7_RA},
    {"d10(RA)", Operand::d10_RA},
    {"rel16", Operand::rel16},
    {"abs16", Operand::abs16},
    {"brinst", Operand::brinst},
    {"scale155", Operand::scale155},
    {"scale173", Operand::scale173},
    {"CA", Operand::CA},
    {"SA", Operand::SA},
    {"stop_type", Operand::stop_type},
};

Operand operand_from_name(std::string_view name) {
    for (const auto& [text, operand] : k_operand_names) {
        if (text == name) return operand;
    }
    throw std::invalid_argument("unknown operand name: " + std::string(name));
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
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

} // namespace spu
