#include "loader/process_info.h"

#include <format>

namespace loader {

namespace {

constexpr u32 k_process_param_magic = 0x13bcc5f6; // PSL1GHT, sys/process.h
constexpr u32 k_prx_param_magic = 0x1b434cec;     // PSL1GHT, crt/lv2-sprx.S
constexpr u64 k_max_module_name_length = 64;

const ElfSegment* find_segment(const ElfFile& elf, u32 type) {
    for (const ElfSegment& segment : elf.segments) {
        if (segment.type == type && segment.file_size != 0) return &segment;
    }
    return nullptr;
}

} // namespace

std::optional<ProcessParam> read_process_param(const ElfFile& elf, const Memory& memory) {
    // sys_process_param_t: {u32 size, u32 magic, u32 version, u32 sdk_version,
    //                       s32 primary_priority, u32 primary_stack_size, ...}
    const ElfSegment* segment = find_segment(elf, k_segment_proc_param);
    if (!segment || segment->file_size < 24) return std::nullopt;
    if (memory.read<u32>(segment->vaddr + 4) != k_process_param_magic) return std::nullopt;

    return ProcessParam{
        .primary_priority = static_cast<s32>(memory.read<u32>(segment->vaddr + 16)),
        .primary_stack_size = memory.read<u32>(segment->vaddr + 20),
    };
}

std::vector<ImportedFunction> read_imports(const ElfFile& elf, const Memory& memory) {
    // sys_proc_prx_param: {u32 size, u32 magic, u32 version, u32 unknown,
    //                      u32 exports_start, u32 exports_end, u32 imports_start, u32 imports_end}
    const ElfSegment* segment = find_segment(elf, k_segment_prx_param);
    if (!segment) return {};
    const u64 param = segment->vaddr;
    if (memory.read<u32>(param + 4) != k_prx_param_magic) {
        throw LoadError("PRX_PARAM segment has the wrong magic number");
    }
    const u32 imports_start = memory.read<u32>(param + 24);
    const u32 imports_end = memory.read<u32>(param + 28);

    // Each library's stub entry: {u8 entry_size, ..., u16 function_count at +6,
    //   u32 module_name at +16, u32 nid_table at +20, u32 stub_table at +24, ...}
    // PSL1GHT, ppu/sprx/common/libexport.c.
    std::vector<ImportedFunction> imports;
    for (u64 entry = imports_start; entry < imports_end;) {
        const u8 entry_size = memory.read<u8>(entry);
        if (entry_size == 0) throw LoadError(std::format("bad import entry at {:#x}", entry));

        const u16 count = memory.read<u16>(entry + 6);
        const std::string module =
            memory.read_string(memory.read<u32>(entry + 16), k_max_module_name_length);
        const u32 nid_table = memory.read<u32>(entry + 20);
        const u32 stub_table = memory.read<u32>(entry + 24);

        for (u32 i = 0; i < count; ++i) {
            imports.push_back({
                .module = module,
                .nid = memory.read<u32>(nid_table + 4 * i),
                .stub_address = stub_table + 4 * i,
            });
        }
        entry += entry_size;
    }
    return imports;
}

} // namespace loader
