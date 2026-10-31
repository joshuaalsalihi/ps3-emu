#pragma once

// Kernel objects that guest code refers to by ID.

#include "common/types.h"

namespace lv2 {

// Synchronization protocols: the order in which waiters are woken. PSL1GHT, sys/mutex.h.
constexpr u32 k_protocol_fifo = 1;
constexpr u32 k_protocol_priority = 2;
constexpr u32 k_protocol_priority_inherit = 3; // treated like k_protocol_priority

constexpr u32 k_mutex_recursive = 0x10;
constexpr u32 k_mutex_not_recursive = 0x20;

struct KernelObject {
    virtual ~KernelObject() = default;
};

struct Mutex : KernelObject {
    bool recursive = false;
    bool wake_by_priority = false;
    u32 owner = 0; // thread ID, or 0 when unlocked
    u32 lock_count = 0;
    u32 condition_variables = 0; // bound condition variables; the mutex can't be destroyed until 0
};

struct Cond : KernelObject {
    u32 mutex = 0;
};

struct Semaphore : KernelObject {
    s32 value = 0;
    s32 max = 0;
    bool wake_by_priority = false;
};

// Physical memory from sys_mmapper_allocate_memory, which can then be mapped at an address.
struct MemoryBlock : KernelObject {
    u64 size = 0;
    u64 page_size = 0;
    u64 mapped_at = 0; // 0 when not mapped
};

} // namespace lv2
