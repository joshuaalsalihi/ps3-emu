#include "hle/linker.h"

#include "common/log.h"
#include "hle/nid.h"

#include <format>

namespace hle {

namespace {

constexpr u64 k_entry_size = 16;           // thunk (8 bytes) + descriptor (8 bytes)
constexpr u32 k_blr = 0x4e800020;          // bclr 20, 0
constexpr u32 k_hle_opcode = u32{1} << 26; // primary opcode 1: the emulator's `hle`

const FunctionInfo* find_function(std::string_view module, u32 nid_value) {
    for (const ModuleInfo& info : modules()) {
        if (info.name != module) continue;
        for (const FunctionInfo& function : info.functions) {
            if (nid(function.name) == nid_value) return &function;
        }
    }
    return nullptr;
}

} // namespace

const std::vector<ModuleInfo>& modules() {
    static const std::vector<ModuleInfo> list = {
        {"sysPrxForUser", sys_prx_for_user_functions()},
    };
    return list;
}

void Linker::link(const std::vector<loader::ImportedFunction>& imports, Memory& memory) {
    if (imports.empty()) return;
    const u64 used = imports.size() * k_entry_size;
    if (used > k_area_size) throw std::runtime_error("too many imported functions");

    const u64 mapped = (used + Memory::k_page_size - 1) & ~(Memory::k_page_size - 1);
    memory.map(k_area, mapped, Memory::k_read_write);

    for (const loader::ImportedFunction& import : imports) {
        const u32 index = static_cast<u32>(bindings_.size());
        const u64 thunk = k_area + index * k_entry_size;
        const u64 descriptor = thunk + 8;

        memory.write<u32>(thunk, k_hle_opcode | index);
        memory.write<u32>(thunk + 4, k_blr);
        memory.write<u32>(descriptor, static_cast<u32>(thunk)); // code address
        memory.write<u32>(descriptor + 4, 0);                   // TOC: unused by thunks
        memory.write<u32>(import.stub_address, static_cast<u32>(descriptor));

        bindings_.push_back({import.module, import.nid, find_function(import.module, import.nid)});
    }
    memory.protect(k_area, mapped, Memory::k_read);
}

void Linker::call(u32 index, lv2::Kernel& kernel, lv2::PpuThread& thread) {
    if (index >= bindings_.size()) {
        log_warning("hle",
                    std::format("no HLE function {} at {:#x}", index, thread.cpu.state.pc - 4));
        return thread.set_result(lv2::k_error_not_implemented);
    }

    Binding& binding = bindings_[index];
    if (!binding.function) {
        // Missing functions report success, so programs can get further; warn once each.
        if (!binding.warned) log_warning("hle", "unimplemented function " + name(index));
        binding.warned = true;
        return thread.set_result(lv2::k_ok);
    }

    try {
        binding.function->function(kernel, thread);
    } catch (const MemoryFault&) {
        thread.set_result(lv2::k_error_bad_address);
    }
}

std::string Linker::name(u32 index) const {
    if (index >= bindings_.size()) return std::format("<hle {}>", index);
    const Binding& binding = bindings_[index];
    if (binding.function) return std::format("{}.{}", binding.module, binding.function->name);
    return std::format("{}.{:#010x}", binding.module, binding.nid);
}

} // namespace hle
