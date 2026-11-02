#include "debug/trace_diff.h"

#include <deque>
#include <format>
#include <istream>

namespace debug {

namespace {

// Traced PPU instructions start with their address, SPU ones with "spu" and theirs.
bool is_instruction(const std::string& line) {
    return line.starts_with("0x") || line.starts_with("spu 0x");
}

std::optional<std::string> next_line(std::istream& stream) {
    std::string line;
    if (!std::getline(stream, line)) return std::nullopt;
    return line;
}

} // namespace

std::optional<TraceDifference> diff_traces(std::istream& first, std::istream& second,
                                           std::size_t context_lines) {
    TraceDifference difference;
    std::deque<std::string> context;
    for (u64 line = 1;; ++line) {
        const std::optional<std::string> a = next_line(first);
        const std::optional<std::string> b = next_line(second);
        if (!a && !b) return std::nullopt;
        if (a != b) {
            difference.line = line;
            difference.context.assign(context.begin(), context.end());
            difference.first = a;
            difference.second = b;
            return difference;
        }

        if (is_instruction(*a)) ++difference.instructions;
        if (a->starts_with("--- ")) difference.thread = a->substr(4);
        context.push_back(*a);
        if (context.size() > context_lines) context.pop_front();
    }
}

std::string describe(const TraceDifference& difference) {
    std::string text =
        std::format("traces differ at line {}, after {} instruction{}", difference.line,
                    difference.instructions, difference.instructions == 1 ? "" : "s");
    if (!difference.thread.empty()) text += ", in " + difference.thread;
    text += ":\n";
    for (const std::string& line : difference.context) {
        text += "  " + line + "\n";
    }
    text += "- " + difference.first.value_or("(end of trace)") + "\n";
    text += "+ " + difference.second.value_or("(end of trace)") + "\n";
    return text;
}

} // namespace debug
