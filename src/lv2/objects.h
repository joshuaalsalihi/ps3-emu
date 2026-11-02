#pragma once

// Kernel objects that guest code refers to by ID.

#include "common/types.h"
#include "core/state.h"

#include <deque>

namespace lv2 {

// Synchronization protocols: the order in which waiters are woken. PSL1GHT, sys/mutex.h.
constexpr u32 k_protocol_fifo = 1;
constexpr u32 k_protocol_priority = 2;
constexpr u32 k_protocol_priority_inherit = 3; // treated like k_protocol_priority

constexpr u32 k_mutex_recursive = 0x10;
constexpr u32 k_mutex_not_recursive = 0x20;

// Each kind of object, so a save state can recreate it.
enum class ObjectKind : u32 {
    mutex,
    cond,
    semaphore,
    event_queue,
    event_port,
    memory_block,
    spu_thread_group,
};

struct KernelObject {
    virtual ~KernelObject() = default;
    virtual ObjectKind kind() const = 0;
    virtual void serialize(state::Archive& archive) = 0;
};

struct Mutex : KernelObject {
    bool recursive = false;
    bool wake_by_priority = false;
    u32 owner = 0; // thread ID, or 0 when unlocked
    u32 lock_count = 0;
    u32 condition_variables = 0; // bound condition variables; the mutex can't be destroyed until 0

    ObjectKind kind() const override { return ObjectKind::mutex; }
    void serialize(state::Archive& archive) override {
        archive(recursive, wake_by_priority, owner, lock_count, condition_variables);
    }
};

struct Cond : KernelObject {
    u32 mutex = 0;

    ObjectKind kind() const override { return ObjectKind::cond; }
    void serialize(state::Archive& archive) override { archive(mutex); }
};

struct Semaphore : KernelObject {
    s32 value = 0;
    s32 max = 0;
    bool wake_by_priority = false;

    ObjectKind kind() const override { return ObjectKind::semaphore; }
    void serialize(state::Archive& archive) override { archive(value, max, wake_by_priority); }
};

// Event queue types and port types. PSL1GHT, sys/event_queue.h.
constexpr u32 k_event_queue_ppu = 1; // received by PPU threads (sys_event_queue_receive)
constexpr u32 k_event_queue_spu = 2; // received by SPU threads it is bound to
constexpr u32 k_event_port_local = 1;
constexpr u32 k_event_queue_force_destroy = 1;
constexpr u32 k_max_event_queue_size = 127;

// sys_event_t: what an event queue delivers.
struct Event {
    u64 source = 0;
    u64 data1 = 0;
    u64 data2 = 0;
    u64 data3 = 0;

    void serialize(state::Archive& archive) { archive(source, data1, data2, data3); }
};

struct EventQueue : KernelObject {
    u32 type = k_event_queue_ppu;
    bool wake_by_priority = false;
    u64 key = 0; // 0 for a queue only this process can connect to
    u32 size = 0;
    std::deque<Event> events; // never more than `size`

    ObjectKind kind() const override { return ObjectKind::event_queue; }
    void serialize(state::Archive& archive) override {
        archive(type, wake_by_priority, key, size, events);
    }
};

struct EventPort : KernelObject {
    u64 name = 0;  // sent as each event's source
    u32 queue = 0; // the connected queue, or 0

    ObjectKind kind() const override { return ObjectKind::event_port; }
    void serialize(state::Archive& archive) override { archive(name, queue); }
};

// Physical memory from sys_mmapper_allocate_memory, which can then be mapped at an address.
struct MemoryBlock : KernelObject {
    u64 size = 0;
    u64 page_size = 0;
    u64 mapped_at = 0; // 0 when not mapped

    ObjectKind kind() const override { return ObjectKind::memory_block; }
    void serialize(state::Archive& archive) override { archive(size, page_size, mapped_at); }
};

} // namespace lv2
