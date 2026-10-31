#pragma once

// Information PS3 executables carry for the kernel: process parameters and library imports.
// Both are read from guest memory after the program's segments are loaded.

#include "common/types.h"
#include "core/memory.h"
#include "loader/elf.h"

#include <optional>
#include <string>
#include <vector>

namespace loader {

// From the PROC_PARAM segment (sys_process_param_t). PSL1GHT, sys/process.h.
struct ProcessParam {
    s32 primary_priority = 0;
    u32 primary_stack_size = 0;
};

// One imported library function: the loader stores a function descriptor's address in its stub.
struct ImportedFunction {
    std::string module;
    u32 nid = 0;          // the function's ID: a hash of its name
    u32 stub_address = 0; // where to write the address of the function's descriptor
};

// Returns the process parameters, if the program has a valid PROC_PARAM segment.
std::optional<ProcessParam> read_process_param(const ElfFile& elf, const Memory& memory);

// Lists the functions the program imports, from the stub tables its PRX_PARAM segment points
// to. Throws LoadError if the tables are malformed.
std::vector<ImportedFunction> read_imports(const ElfFile& elf, const Memory& memory);

} // namespace loader
