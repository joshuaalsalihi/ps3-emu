// Memory syscalls. The logic is in kernel_memory.cpp; these read the arguments and write the
// results.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

// Writes `value` to `address` if `result` is success.
void write_on_success(Kernel& kernel, PpuThread& thread, s32 result, u32 address, u32 value) {
    if (result == k_ok) kernel.memory.write<u32>(address, value);
    thread.set_result(result);
}

// int sys_memory_allocate(size_t size, u64 flags, sys_addr_t* address)
void sys_memory_allocate(Kernel& kernel, PpuThread& thread) {
    const u32 address_out = thread.pointer_arg(2);
    if (!kernel.memory.is_accessible(address_out, 4, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }
    u32 address = 0;
    const s32 result = kernel.allocate_memory(thread.arg(0), thread.arg(1), address);
    write_on_success(kernel, thread, result, address_out, address);
}

// int sys_memory_free(sys_addr_t address)
void sys_memory_free(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.free_memory(thread.pointer_arg(0)));
}

// int sys_memory_get_user_memory_size(sys_memory_info_t* info)
// The info is {u32 total, u32 available}.
void sys_memory_get_user_memory_size(Kernel& kernel, PpuThread& thread) {
    const u32 info = thread.pointer_arg(0);
    kernel.memory.write<u32>(info, static_cast<u32>(Kernel::k_user_memory_size));
    kernel.memory.write<u32>(info + 4, static_cast<u32>(kernel.user_memory_available()));
    thread.set_result(k_ok);
}

// int sys_mmapper_allocate_address(size_t size, u64 flags, size_t alignment, sys_addr_t* address)
void sys_mmapper_allocate_address(Kernel& kernel, PpuThread& thread) {
    const u32 address_out = thread.pointer_arg(3);
    if (!kernel.memory.is_accessible(address_out, 4, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }
    u32 address = 0;
    const s32 result = kernel.reserve_address_range(thread.arg(0), thread.arg(2), address);
    write_on_success(kernel, thread, result, address_out, address);
}

// int sys_mmapper_free_address(sys_addr_t start_address)
void sys_mmapper_free_address(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.release_address_range(thread.pointer_arg(0)));
}

// int sys_mmapper_search_and_map(sys_addr_t start_address, sys_memory_t mem_id, u64 flags,
//                                sys_addr_t* address)
void sys_mmapper_search_and_map(Kernel& kernel, PpuThread& thread) {
    const u32 address_out = thread.pointer_arg(3);
    if (!kernel.memory.is_accessible(address_out, 4, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }
    u32 address = 0;
    const s32 result = kernel.search_and_map_memory_block(thread.pointer_arg(0),
                                                          static_cast<u32>(thread.arg(1)), address);
    write_on_success(kernel, thread, result, address_out, address);
}

constexpr SyscallInfo k_syscalls[] = {
    {330, "sys_mmapper_allocate_address", sys_mmapper_allocate_address},
    {331, "sys_mmapper_free_address", sys_mmapper_free_address},
    {337, "sys_mmapper_search_and_map", sys_mmapper_search_and_map},
    {348, "sys_memory_allocate", sys_memory_allocate},
    {349, "sys_memory_free", sys_memory_free},
    {352, "sys_memory_get_user_memory_size", sys_memory_get_user_memory_size},
};

} // namespace

std::span<const SyscallInfo> memory_syscalls() {
    return k_syscalls;
}

} // namespace lv2
