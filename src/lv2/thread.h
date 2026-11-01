#pragma once

#include "common/types.h"
#include "cpu/ppu/thread.h"

#include <optional>
#include <string>
#include <utility>

namespace lv2 {

enum class ThreadStatus {
    stopped,  // created, but not started yet
    ready,    // can run
    waiting,  // blocked until woken, or until its wait times out
    finished, // exited
};

// What a waiting thread is waiting for.
enum class WaitKind { sleep, join, mutex, cond, semaphore, spu_group, event_queue };

struct Wait {
    WaitKind kind = WaitKind::sleep;
    u32 object = 0;              // the mutex, condition variable, semaphore, thread, group or queue
    u64 sequence = 0;            // arrival order, so waiters are woken first come, first served
    std::optional<u64> deadline; // virtual clock cycle at which the wait times out
    u32 output_address = 0;      // join: where to store the exit value (SPU: the cause)
    u32 status_address = 0;      // SPU group join: where to store the exit status

    // A condition variable wait ends by locking its mutex again. These carry over to that phase.
    s32 result = 0;           // the result to report once the mutex is ours
    u32 saved_lock_count = 0; // the recursive lock depth to restore

    static Wait on(WaitKind kind, u32 object = 0) {
        Wait wait;
        wait.kind = kind;
        wait.object = object;
        return wait;
    }
};

// A PPU thread as the kernel sees it: CPU state plus scheduling and lifetime information.
struct PpuThread {
    PpuThread(u32 id, std::string name, s32 priority, Memory& memory, VirtualClock& clock)
        : id(id), name(std::move(name)), priority(priority), cpu(id, memory, clock) {}

    u32 id;
    std::string name;
    s32 priority; // lower numbers are more urgent
    ppu::Thread cpu;

    ThreadStatus status = ThreadStatus::stopped;
    Wait wait; // valid while status is `waiting`

    bool joinable = true;
    u64 exit_value = 0;
    u64 stack_address = 0;
    u64 stack_size = 0;

    // Syscall and HLE function arguments are in r3-r10; the result goes in r3.
    u64 arg(u32 index) const { return cpu.state.gpr[3 + index]; }
    // Pointers are 32-bit guest addresses.
    u32 pointer_arg(u32 index) const { return static_cast<u32>(arg(index)); }
    void set_result(s32 result) { cpu.state.gpr[3] = static_cast<u64>(s64{result}); }
};

} // namespace lv2
