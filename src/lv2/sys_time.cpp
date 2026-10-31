// Timer and time syscalls. All time is virtual: it comes from the emulator's clock, never the
// host's, so runs are deterministic.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

// int sys_timer_usleep(usecond_t microseconds)
void sys_timer_usleep(Kernel& kernel, PpuThread& thread) {
    const u64 microseconds = thread.arg(0);
    if (microseconds == 0) return thread.set_result(k_ok);
    kernel.wait(thread, Wait::on(WaitKind::sleep), microseconds);
}

// int sys_timer_sleep(second_t seconds)
void sys_timer_sleep(Kernel& kernel, PpuThread& thread) {
    const u64 seconds = static_cast<u32>(thread.arg(0));
    if (seconds == 0) return thread.set_result(k_ok);
    kernel.wait(thread, Wait::on(WaitKind::sleep), seconds * 1'000'000);
}

// int sys_time_get_current_time(sys_time_sec_t* seconds, sys_time_nsec_t* nanoseconds)
void sys_time_get_current_time(Kernel& kernel, PpuThread& thread) {
    // 3.2 GHz: one cycle is 5/16 of a nanosecond.
    const u64 elapsed_ns = kernel.clock.cycles() / 16 * 5;
    kernel.memory.write<u64>(thread.pointer_arg(0),
                             Kernel::k_boot_time_seconds + elapsed_ns / 1'000'000'000);
    kernel.memory.write<u64>(thread.pointer_arg(1), elapsed_ns % 1'000'000'000);
    thread.set_result(k_ok);
}

// u64 sys_time_get_timebase_frequency(void)
void sys_time_get_timebase_frequency(Kernel&, PpuThread& thread) {
    thread.cpu.state.gpr[3] = VirtualClock::k_timebase_frequency;
}

constexpr SyscallInfo k_syscalls[] = {
    {141, "sys_timer_usleep", sys_timer_usleep},
    {142, "sys_timer_sleep", sys_timer_sleep},
    {145, "sys_time_get_current_time", sys_time_get_current_time},
    {147, "sys_time_get_timebase_frequency", sys_time_get_timebase_frequency},
};

} // namespace

std::span<const SyscallInfo> time_syscalls() {
    return k_syscalls;
}

} // namespace lv2
