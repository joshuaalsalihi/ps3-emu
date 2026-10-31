// Process syscalls.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

// UNVERIFIED: the process ID LV2 gives a game.
constexpr s32 k_process_id = 1;

// sys_pid_t sys_process_getpid(void)
void sys_process_getpid(Kernel&, PpuThread& thread) {
    thread.set_result(k_process_id);
}

// void sys_process_exit(int status)
void sys_process_exit(Kernel& kernel, PpuThread& thread) {
    kernel.exit_process(static_cast<s32>(thread.arg(0)));
}

constexpr SyscallInfo k_syscalls[] = {
    {1, "sys_process_getpid", sys_process_getpid},
    {3, "sys_process_exit", sys_process_exit},
    {22, "sys_process_exit2", sys_process_exit},
};

} // namespace

std::span<const SyscallInfo> process_syscalls() {
    return k_syscalls;
}

} // namespace lv2
