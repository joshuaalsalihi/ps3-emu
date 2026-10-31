#pragma once

#include "common/types.h"

#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace spu {

class AssemblyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Assembles one instruction located at `pc`, using the syntax the disassembler prints (spu-as
// syntax): "a $3, $4, $5", "lqd $3, 16($1)", "brz $3, 0x40", "rdch $3, $ch29". Branch targets
// are absolute addresses. Throws AssemblyError on invalid input.
u32 assemble(std::string_view line, u32 pc = 0);

// Assembles consecutive instructions starting at `base`.
std::vector<u32> assemble(std::span<const std::string_view> lines, u32 base = 0);

} // namespace spu
