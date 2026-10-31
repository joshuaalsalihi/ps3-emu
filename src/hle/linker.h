#pragma once

// Connects a program's imported library functions to their high-level emulated versions.
//
// Each import gets a two-instruction thunk, `hle n; blr`, plus a PS3 function descriptor that
// points at the thunk. The descriptor's address goes in the program's stub for that import, so a
// call to the import runs the thunk, which stops the interpreter and runs HLE function n.

#include "common/types.h"
#include "core/memory.h"
#include "loader/process_info.h"
#include "lv2/kernel.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hle {

// An HLE function. Like a syscall, it reads its arguments from the thread, then either sets a
// result in r3 or makes the thread wait.
using Function = void (*)(lv2::Kernel&, lv2::PpuThread&);

struct FunctionInfo {
    std::string_view name; // the official name, which determines the function's NID
    Function function;
};

struct ModuleInfo {
    std::string_view name;
    std::span<const FunctionInfo> functions;
};

// The libraries with HLE functions, each defined in its own file.
std::span<const FunctionInfo> sys_prx_for_user_functions();
const std::vector<ModuleInfo>& modules();

class Linker {
public:
    // Where thunks and descriptors live. UNVERIFIED: an emulator-owned area, chosen to stay
    // clear of the ranges the kernel hands out.
    static constexpr u64 k_area = 0x2f00'0000;
    static constexpr u64 k_area_size = 0x00ff'0000;

    void link(const std::vector<loader::ImportedFunction>& imports, Memory& memory);

    // Runs the function behind `hle index` for `thread`.
    void call(u32 index, lv2::Kernel& kernel, lv2::PpuThread& thread);

    // "sysPrxForUser.sys_lwmutex_lock", or the module and NID if the name is unknown.
    std::string name(u32 index) const;

private:
    struct Binding {
        std::string module;
        u32 nid;
        const FunctionInfo* function; // null if not implemented
        bool warned = false;
    };
    std::vector<Binding> bindings_;
};

} // namespace hle
