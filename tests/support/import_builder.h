#pragma once

// Builds the import tables a PS3 executable carries, in guest memory, the way PSL1GHT's linker
// lays them out: a sys_proc_prx_param block pointing at one 44-byte stub entry per library.

#include "core/memory.h"

#include <string_view>
#include <vector>

namespace test {

struct ImportTables {
    u64 prx_param;               // where the PRX_PARAM segment should point
    std::vector<u64> stub_slots; // one per function, where the loader stores descriptors
};

// Writes tables importing `nids` from `module`, starting at `base`. Uses about 0x200 bytes.
inline ImportTables write_import_tables(Memory& memory, u64 base, std::string_view module,
                                        const std::vector<u32>& nids) {
    const u64 prx_param = base;
    const u64 stub_entry = base + 0x40;
    const u64 name = base + 0x80;
    const u64 nid_table = base + 0xc0;
    const u64 stub_table = base + 0x140;

    memory.write<u32>(prx_param + 0, 0x28);       // size
    memory.write<u32>(prx_param + 4, 0x1b434cec); // magic
    memory.write<u32>(prx_param + 24, static_cast<u32>(stub_entry));
    memory.write<u32>(prx_param + 28, static_cast<u32>(stub_entry + 0x2c));

    memory.write<u32>(stub_entry + 0, 0x2c000001); // entry size 0x2c, version 1
    memory.write<u16>(stub_entry + 4, 0x0009);
    memory.write<u16>(stub_entry + 6, static_cast<u16>(nids.size()));
    memory.write<u32>(stub_entry + 16, static_cast<u32>(name));
    memory.write<u32>(stub_entry + 20, static_cast<u32>(nid_table));
    memory.write<u32>(stub_entry + 24, static_cast<u32>(stub_table));

    for (std::size_t i = 0; i < module.size(); ++i) {
        memory.write<u8>(name + i, static_cast<u8>(module[i]));
    }

    ImportTables tables{prx_param, {}};
    for (std::size_t i = 0; i < nids.size(); ++i) {
        memory.write<u32>(nid_table + 4 * i, nids[i]);
        tables.stub_slots.push_back(stub_table + 4 * i);
    }
    return tables;
}

} // namespace test
