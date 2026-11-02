#pragma once

// SPU threads and SPU thread groups: LV2's way of running programs on the SPUs. A group holds a
// fixed number of threads, which start and stop together.

#include "common/types.h"
#include "cpu/spu/spe.h"
#include "lv2/objects.h"
#include "lv2/thread.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lv2 {

// sys_spu_thread_group_join's causes. PSL1GHT, sys/spu.h.
constexpr u32 k_spu_join_group_exit = 0x1;       // a thread called spu_thread_group_exit
constexpr u32 k_spu_join_all_threads_exit = 0x2; // every thread called spu_thread_exit
constexpr u32 k_spu_join_terminated = 0x4;       // the PPU called sys_spu_thread_group_terminate

// Stop signals with a meaning to LV2. The exit status is in the outbound mailbox.
// PSL1GHT's libsputhread: spu_thread_group_exit, spu_thread_exit and spu_thread_group_yield.
constexpr u32 k_spu_stop_group_exit = 0x101;
constexpr u32 k_spu_stop_thread_exit = 0x102;
constexpr u32 k_spu_stop_yield = 0x103;
// Receiving from an event queue bound to the thread: the binding's number is in the outbound
// mailbox, and the result, then the event's three data words, come back in the inbound mailbox.
// PSL1GHT's libsputhread: spu_thread_receive_event and spu_thread_tryreceive_event.
constexpr u32 k_spu_stop_receive_event = 0x110;
constexpr u32 k_spu_stop_try_receive_event = 0x111;

// SPU thread events: sent by writing SPU_WrOutIntrMbox, to the queue connected to one of the
// thread's 64 ports. PSL1GHT, sys/spu.h and the SPU's sys/spu_event.h.
constexpr u32 k_spu_thread_event_user = 1;
constexpr u32 k_spu_thread_event_dma = 2;
constexpr u64 k_spu_thread_event_user_key = 0xffff'ffff'5350'5501; // the events' source
constexpr u32 k_spu_event_port_count = 64;

// How a thread's local store is set up: segments copied from guest memory or filled with a
// value. PSL1GHT, lv2/spu.h: sys_spu_segment_t.
constexpr u32 k_spu_segment_copy = 1;
constexpr u32 k_spu_segment_fill = 2;
constexpr u32 k_spu_segment_info = 4;

struct SpuSegment {
    u32 type = 0;
    u32 local_address = 0;
    u32 size = 0;
    u32 source_or_value = 0; // copy: guest address; fill: the 32-bit value
};

struct SpuImage {
    u32 entry = 0;
    std::vector<SpuSegment> segments;
};

struct SpuThread {
    u32 id = 0;
    std::string name;
    u32 group = 0;
    u32 index = 0; // the thread's number within its group

    // Created when the thread is initialized, and again each time its group starts.
    std::unique_ptr<spu::Spe> spe;
    ThreadStatus status = ThreadStatus::stopped;
    std::optional<u32> blocked_channel; // while waiting: the channel it stalled on
    u32 waiting_queue = 0;              // while waiting: the event queue it receives from
    u64 wait_sequence = 0;              // arrival order among a queue's waiters

    // What each start restores: the loaded image and the arguments for $3-$6.
    std::vector<u8> initial_local_store;
    u32 entry = 0;
    std::array<u64, 4> arguments{};
    bool needs_reload = false; // the local store no longer holds the fresh image

    std::optional<s32> exit_status; // set by spu_thread_exit

    // Event queues: the one connected to each SPU port (0 if none), and those bound for
    // receiving, by binding number. These outlast restarts of the group.
    std::array<u32, k_spu_event_port_count> event_ports{};
    std::map<u32, u32> bound_queues;

    // The SPE, which needs the kernel's memory and clock, is saved by the kernel.
    void serialize(state::Archive& archive) {
        archive(id, name, group, index, status, blocked_channel, waiting_queue, wait_sequence,
                initial_local_store, entry, arguments, needs_reload, exit_status, event_ports,
                bound_queues);
    }
};

struct SpuThreadGroup : KernelObject {
    enum class Status { not_initialized, initialized, running };

    std::string name;
    u32 size = 0;
    s32 priority = 0;
    std::vector<u32> threads; // thread IDs by index; 0 while not initialized
    Status status = Status::not_initialized;

    // The last run's outcome, until a join collects it.
    struct Exit {
        u32 cause = 0;
        s32 status = 0;

        void serialize(state::Archive& archive) { archive(cause, status); }
    };
    std::optional<Exit> exit;
    bool has_joiner = false;

    ObjectKind kind() const override { return ObjectKind::spu_thread_group; }
    void serialize(state::Archive& archive) override {
        archive(name, size, priority, threads, status, exit, has_joiner);
    }
};

} // namespace lv2
