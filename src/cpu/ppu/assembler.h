#pragma once

#include "common/types.h"

#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ppu {

class AssemblyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Assembles one instruction located at `pc`, using the syntax the disassembler prints:
// "addi r3, r1, -8", "lwz r4, 16(r1)", "beq cr7, 0x10040", "li r3, 1".
// Branch targets are absolute addresses. Throws AssemblyError on invalid input.
u32 assemble(std::string_view line, u64 pc = 0);

// Assembles consecutive instructions starting at `base`.
std::vector<u32> assemble(std::span<const std::string_view> lines, u64 base = 0);

} // namespace ppu
