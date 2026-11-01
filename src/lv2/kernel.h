#pragma once

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "core/range_allocator.h"
#include "lv2/errors.h"
#include "lv2/objects.h"
#include "lv2/spu_thread.h"
#include "lv2/thread.h"

#include <array>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rsx {
class Rsx;
}

namespace lv2 {

class Kernel;

// A syscall implementation. It reads its arguments from the thread, then either sets a result or
// makes the thread wait (the result is then set when it wakes).
using SyscallHandler = void (*)(Kernel&, PpuThread&);

struct SyscallInfo {
    u32 number;
    std::string_view name;
    SyscallHandler handler;
};

// Syscall tables, defined next to their handlers. Numbers: psdevwiki, "LV2 Functions and
// Syscalls".
std::span<const SyscallInfo> process_syscalls();
std::span<const SyscallInfo> thread_syscalls();
std::span<const SyscallInfo> sync_syscalls();
std::span<const SyscallInfo> time_syscalls();
std::span<const SyscallInfo> memory_syscalls();
std::span<const SyscallInfo> tty_syscalls();
std::span<const SyscallInfo> fs_syscalls();
std::span<const SyscallInfo> spu_syscalls();
std::span<const SyscallInfo> event_syscalls();

// A thread the scheduler can run next: a PPU thread or an SPU thread.
struct Runnable {
    PpuThread* ppu = nullptr;
    SpuThread* spu = nullptr;

    explicit operator bool() const { return ppu || spu; }
};

// High-level emulation of the LV2 kernel: threads, scheduling, synchronization and memory.
//
// Scheduling is deterministic. Threads take turns round-robin, each running for a fixed slice
// of instructions or until it makes a syscall. Waiting threads are woken first come, first
// served (or by priority, if the object asks for that), and timeouts are measured on the
// virtual clock.
class Kernel {
public:
    // Guest address ranges the kernel hands out. UNVERIFIED: modeled on the user memory layout
    // psdevwiki describes; the exact boundaries may differ.
    static constexpr u64 k_stub_page = 0x2fff'0000;        // the kernel's own code stubs
    static constexpr u64 k_user_memory_base = 0x3000'0000; // sys_memory_allocate
    static constexpr u64 k_user_memory_size = 0x1000'0000; // 256 MB
    static constexpr u64 k_mmapper_base = 0x4000'0000;     // sys_mmapper address ranges
    static constexpr u64 k_mmapper_size = 0x8000'0000;
    static constexpr u64 k_stack_area_base = 0xd000'0000; // thread stacks
    static constexpr u64 k_stack_area_size = 0x1000'0000;

    // Where threads return to when their entry function returns: stubs that call
    // sys_ppu_thread_exit or sys_process_exit with the return value.
    static constexpr u64 k_thread_exit_stub = k_stub_page;
    static constexpr u64 k_process_exit_stub = k_stub_page + 8;

    // Guest code runs this many instructions before another ready thread gets a turn.
    // UNVERIFIED: LV2's real time slice is unknown; this only needs to be fair and fixed.
    static constexpr u64 k_time_slice = 1000;

    // sys_time_get_current_time counts from here, so runs don't depend on the host clock.
    static constexpr u64 k_boot_time_seconds = 1'163'203'200; // 2006-11-11, the PS3's launch

    Kernel(Memory& memory, VirtualClock& clock, std::ostream& tty);

    Memory& memory;
    VirtualClock& clock;
    std::ostream& tty;
    rsx::Rsx* rsx = nullptr; // the GPU, which libgcm's HLE functions drive

    // ---- Syscalls ----

    // Handles the `sc` instruction `thread` just executed: the syscall number is in r11.
    void handle_syscall(PpuThread& thread);
    const SyscallInfo* find_syscall(u32 number) const;

    // ---- Process ----

    void exit_process(s32 status);
    std::optional<s32> exit_status() const { return exit_status_; }

    // ---- Threads and scheduling (kernel.cpp) ----

    // The program's thread-local storage template (its TLS segment). Every thread created
    // afterwards gets its own copy, with r13 pointing at it.
    void set_tls_template(u64 address, u64 file_size, u64 memory_size);

    // Creates a stopped thread that will call the function at `entry` with `argument` in r3.
    // Returns null if there is no room for its stack.
    PpuThread* create_thread(std::string name, u64 entry, u64 toc, u64 argument, s32 priority,
                             u64 stack_size);
    void start_thread(PpuThread& thread);
    // Creates a stopped thread from a PS3 function descriptor (32-bit code address, then TOC),
    // checking its arguments as sys_ppu_thread_create does. Flag bit 0 makes it joinable.
    s32 create_thread_from_descriptor(u32 descriptor, u64 argument, s32 priority, u64 stack_size,
                                      u64 flags, std::string name, u32& thread_id);
    void exit_thread(PpuThread& thread, u64 exit_value);
    PpuThread* find_thread(u32 id);
    const std::vector<std::unique_ptr<PpuThread>>& threads() const { return threads_; }
    bool all_threads_finished() const;

    // The next thread to run, taking turns: PPU threads in creation order, then SPU threads in
    // creation order. Empty if none is ready.
    Runnable next_thread();

    // Makes `thread` wait. `timeout_us` of 0 means no timeout, as in LV2's own syscalls.
    void wait(PpuThread& thread, Wait wait, u64 timeout_us);
    // Ends a thread's wait and gives it `result`.
    void wake(PpuThread& thread, s32 result);
    // The thread to wake next from `object`: the earliest to arrive, or the most urgent priority.
    PpuThread* first_waiter(WaitKind kind, u32 object, bool by_priority);
    bool has_waiters(WaitKind kind, u32 object) const;

    // Ends waits whose timeout has passed.
    void wake_timed_out_threads();
    // When no thread is ready, moves the clock forward to the next timeout, or to `device_deadline`
    // (a device's next event, such as a vblank) if that is sooner. Returns false if there is
    // neither.
    bool skip_to_next_timeout(std::optional<u64> device_deadline = std::nullopt);
    // One line per waiting thread, for deadlock reports.
    std::string describe_waits() const;

    // ---- SPU threads (kernel_spu.cpp) ----

    // SPUs available to programs: six of the Cell's eight. PSL1GHT, sys/spu.h.
    static constexpr u32 k_spu_count = 6;
    // UNVERIFIED: the priority range for SPU thread groups (lower numbers are more urgent).
    static constexpr s32 k_min_spu_priority = 16;
    static constexpr s32 k_max_spu_priority = 255;

    // Debug option for new SPU threads: DMA completes the moment it is issued.
    bool instant_dma = false;

    s32 create_spu_thread_group(u32 size, s32 priority, std::string name, u32& group_id);
    s32 destroy_spu_thread_group(u32 group_id);
    // Creates thread `index` of a group and loads `image` into its local store.
    s32 initialize_spu_thread(u32 group_id, u32 index, const SpuImage& image, std::string name,
                              const std::array<u64, 4>& arguments, u32& thread_id);
    s32 start_spu_thread_group(u32 group_id);
    s32 terminate_spu_thread_group(u32 group_id, s32 value);
    // Waits for the group's threads to end, then writes the cause and status. Returns a result,
    // or nothing if the thread now waits.
    std::optional<s32> join_spu_thread_group(PpuThread& thread, u32 group_id, u32 cause_address,
                                             u32 status_address);

    SpuThread* find_spu_thread(u32 id);
    const std::vector<std::unique_ptr<SpuThread>>& spu_threads() const { return spu_threads_; }

    // An SPU thread executed `stop`. Returns false if LV2 gives the signal no meaning, which
    // ends the run with a fault.
    bool handle_spu_stop(SpuThread& thread, u32 signal);
    // An SPU thread stalled on a channel; it waits until the access can complete.
    void block_spu_thread(SpuThread& thread, u32 channel);
    // Lets SPU DMA progress, and wakes SPU threads whose channel access can now complete.
    void update_spu_threads();

    // ---- Objects (kernel.cpp) ----

    u32 add_object(std::unique_ptr<KernelObject> object);
    void remove_object(u32 id);

    template <typename T>
    T* find_object(u32 id) {
        const auto it = objects_.find(id);
        return it == objects_.end() ? nullptr : dynamic_cast<T*>(it->second.get());
    }

    // ---- Synchronization (kernel_sync.cpp) ----
    // These return a result, or nothing if the thread now waits (it gets its result on waking).

    std::optional<s32> lock_mutex(PpuThread& thread, u32 mutex_id, u64 timeout_us);
    s32 try_lock_mutex(PpuThread& thread, u32 mutex_id);
    s32 unlock_mutex(PpuThread& thread, u32 mutex_id);
    s32 destroy_mutex(u32 mutex_id);

    s32 create_cond(u32 mutex_id, u32& cond_id);
    std::optional<s32> wait_cond(PpuThread& thread, u32 cond_id, u64 timeout_us);
    s32 signal_cond(PpuThread& thread, u32 cond_id);
    s32 signal_cond_all(PpuThread& thread, u32 cond_id);
    s32 signal_cond_to(PpuThread& thread, u32 cond_id, u32 thread_id);
    s32 destroy_cond(u32 cond_id);

    std::optional<s32> wait_semaphore(PpuThread& thread, u32 semaphore_id, u64 timeout_us);
    s32 try_wait_semaphore(u32 semaphore_id);
    s32 post_semaphore(u32 semaphore_id, s32 count);
    s32 destroy_semaphore(u32 semaphore_id);

    // ---- Event queues and ports (kernel_event.cpp) ----

    s32 create_event_queue(u32 protocol, u32 type, u64 key, s32 size, u32& queue_id);
    s32 destroy_event_queue(u32 queue_id, u32 mode);
    // Takes the oldest event, or waits for one. A PPU thread gets its event in r4-r7.
    std::optional<s32> receive_event(PpuThread& thread, u32 queue_id, u64 timeout_us);
    // Takes up to `max` waiting events without waiting.
    s32 try_receive_events(u32 queue_id, u32 max, std::vector<Event>& events);
    s32 drain_event_queue(u32 queue_id);
    // Delivers an event to the queue's first waiter, or keeps it. EBUSY if the queue is full.
    s32 send_event(u32 queue_id, const Event& event);

    s32 create_event_port(u32 type, u64 name, u32& port_id);
    s32 destroy_event_port(u32 port_id);
    s32 connect_event_port(u32 port_id, u32 queue_id);
    s32 disconnect_event_port(u32 port_id);
    s32 send_event_from_port(u32 port_id, u64 data1, u64 data2, u64 data3);

    // SPU threads send events through their ports and receive them from bound queues.
    s32 connect_spu_thread_event(u32 thread_id, u32 queue_id, u32 type, u32 port);
    s32 disconnect_spu_thread_event(u32 thread_id, u32 type, u32 port);
    s32 bind_spu_thread_queue(u32 thread_id, u32 queue_id, u32 number);
    s32 unbind_spu_thread_queue(u32 thread_id, u32 number);
    // The SPU thread wrote SPU_WrOutIntrMbox, which interrupts the PPU: LV2 sends the event
    // it describes.
    void handle_spu_interrupt(SpuThread& thread);

    // ---- Memory (kernel_memory.cpp) ----

    // sys_memory_allocate: maps fresh memory somewhere in user memory.
    s32 allocate_memory(u64 size, u64 flags, u32& address);
    s32 free_memory(u32 address);
    u64 user_memory_available() const;

    // sys_mmapper: reserve address ranges, create memory blocks, and map blocks into ranges.
    s32 reserve_address_range(u64 size, u64 alignment, u32& address);
    s32 release_address_range(u32 address);
    s32 create_memory_block(u64 size, u64 flags, u32& block_id);
    s32 destroy_memory_block(u32 block_id);
    s32 map_memory_block(u32 address, u32 block_id);
    s32 search_and_map_memory_block(u32 range_start, u32 block_id, u32& address);
    s32 unmap_memory_block(u32 address, u32& block_id);

private:
    void finish_cond_wait(PpuThread& waiter, s32 result);
    void hand_off_mutex(Mutex& mutex, u32 mutex_id);
    u64 page_size_from_flags(u64 flags) const;

    std::map<u32, const SyscallInfo*> syscalls_;

    void load_spu_thread(SpuThread& thread);
    void receive_spu_event(SpuThread& thread, bool can_wait);
    SpuThread* first_spu_waiter(u32 queue_id);
    void end_spu_thread_group(SpuThreadGroup& group, u32 group_id, u32 cause, s32 status);

    std::vector<std::unique_ptr<PpuThread>> threads_;     // in creation order
    std::vector<std::unique_ptr<SpuThread>> spu_threads_; // in creation order
    std::size_t next_to_schedule_ = 0; // where the round-robin search starts, over both lists
    u64 tls_address_ = 0;
    u64 tls_file_size_ = 0;
    u64 tls_memory_size_ = 0;
    u64 next_wait_sequence_ = 0;

    std::map<u32, std::unique_ptr<KernelObject>> objects_;
    u32 next_id_ = 0x1000; // IDs are handed out in order, so runs are reproducible

    RangeAllocator stacks_{k_stack_area_base, k_stack_area_size};
    RangeAllocator user_memory_{k_user_memory_base, k_user_memory_size};
    RangeAllocator mmapper_ranges_{k_mmapper_base, k_mmapper_size};
    std::map<u64, RangeAllocator> mapped_blocks_; // per reserved range: where blocks are mapped

    std::optional<s32> exit_status_;
};

} // namespace lv2
