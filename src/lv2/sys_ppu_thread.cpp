// PPU thread syscalls.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

constexpr u64 k_max_thread_name_length = 27;

u32 thread_id_arg(const PpuThread& thread, u32 index) {
    return static_cast<u32>(thread.arg(index)); // sys_ppu_thread_t is 64-bit; our IDs fit in 32
}

// int sys_ppu_thread_create(sys_ppu_thread_t* thread_id, const param* param, u64 arg,
//                           int unused, int prio, size_t stacksize, u64 flags, const char* name)
// UNVERIFIED: psdevwiki shows `param` only as "& entry"; we read it as {u32 entry descriptor,
// u32 TLS address}, the layout the SDK's sys_ppu_thread_create wrapper would pass.
void sys_ppu_thread_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const u32 descriptor = kernel.memory.read<u32>(thread.pointer_arg(1));
    const std::string name =
        thread.pointer_arg(7) == 0
            ? ""
            : kernel.memory.read_string(thread.pointer_arg(7), k_max_thread_name_length);
    if (!kernel.memory.is_accessible(id_out, 8, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }

    u32 new_id = 0;
    const s32 result = kernel.create_thread_from_descriptor(
        descriptor, thread.arg(2), static_cast<s32>(thread.arg(4)), thread.arg(5), thread.arg(6),
        name, new_id);
    if (result == k_ok) kernel.memory.write<u64>(id_out, new_id);
    thread.set_result(result);
}

// int sys_ppu_thread_start(sys_ppu_thread_t thread_id)
void sys_ppu_thread_start(Kernel& kernel, PpuThread& thread) {
    PpuThread* target = kernel.find_thread(thread_id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    if (target->status != ThreadStatus::stopped) return thread.set_result(k_error_bad_state);

    kernel.start_thread(*target);
    thread.set_result(k_ok);
}

// void sys_ppu_thread_exit(u64 exit_value)
void sys_ppu_thread_exit(Kernel& kernel, PpuThread& thread) {
    kernel.exit_thread(thread, thread.arg(0));
}

// void sys_ppu_thread_yield(void)
// The scheduler already gives another thread a turn after every syscall.
void sys_ppu_thread_yield(Kernel&, PpuThread& thread) {
    thread.set_result(k_ok);
}

// int sys_ppu_thread_join(sys_ppu_thread_t thread_id, u64* exit_value)
void sys_ppu_thread_join(Kernel& kernel, PpuThread& thread) {
    PpuThread* target = kernel.find_thread(thread_id_arg(thread, 0));
    const u32 exit_value_out = thread.pointer_arg(1);
    if (!target) return thread.set_result(k_error_no_such_id);
    if (target == &thread) return thread.set_result(k_error_deadlock);
    if (!target->joinable) return thread.set_result(k_error_invalid);

    if (target->status == ThreadStatus::finished) {
        if (exit_value_out != 0) kernel.memory.write<u64>(exit_value_out, target->exit_value);
        target->joinable = false;
        return thread.set_result(k_ok);
    }
    Wait join = Wait::on(WaitKind::join, target->id);
    join.output_address = exit_value_out;
    kernel.wait(thread, join, 0);
}

// int sys_ppu_thread_detach(sys_ppu_thread_t thread_id)
void sys_ppu_thread_detach(Kernel& kernel, PpuThread& thread) {
    PpuThread* target = kernel.find_thread(thread_id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    if (!target->joinable) return thread.set_result(k_error_invalid);
    if (kernel.has_waiters(WaitKind::join, target->id)) return thread.set_result(k_error_busy);

    target->joinable = false;
    thread.set_result(k_ok);
}

// void sys_ppu_thread_get_join_state(int* is_joinable)
void sys_ppu_thread_get_join_state(Kernel& kernel, PpuThread& thread) {
    kernel.memory.write<u32>(thread.pointer_arg(0), thread.joinable ? 1 : 0);
    thread.set_result(k_ok);
}

// int sys_ppu_thread_set_priority(sys_ppu_thread_t thread_id, int priority)
void sys_ppu_thread_set_priority(Kernel& kernel, PpuThread& thread) {
    PpuThread* target = kernel.find_thread(thread_id_arg(thread, 0));
    const s32 priority = static_cast<s32>(thread.arg(1));
    if (!target) return thread.set_result(k_error_no_such_id);
    if (priority < 0 || priority > 3071) return thread.set_result(k_error_invalid);

    target->priority = priority;
    thread.set_result(k_ok);
}

// int sys_ppu_thread_get_priority(sys_ppu_thread_t thread_id, int* priority)
void sys_ppu_thread_get_priority(Kernel& kernel, PpuThread& thread) {
    PpuThread* target = kernel.find_thread(thread_id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);

    kernel.memory.write<u32>(thread.pointer_arg(1), static_cast<u32>(target->priority));
    thread.set_result(k_ok);
}

// int sys_ppu_thread_rename(sys_ppu_thread_t thread_id, const char* name)
void sys_ppu_thread_rename(Kernel& kernel, PpuThread& thread) {
    PpuThread* target = kernel.find_thread(thread_id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);

    target->name = kernel.memory.read_string(thread.pointer_arg(1), k_max_thread_name_length);
    thread.set_result(k_ok);
}

constexpr SyscallInfo k_syscalls[] = {
    {41, "sys_ppu_thread_exit", sys_ppu_thread_exit},
    {43, "sys_ppu_thread_yield", sys_ppu_thread_yield},
    {44, "sys_ppu_thread_join", sys_ppu_thread_join},
    {45, "sys_ppu_thread_detach", sys_ppu_thread_detach},
    {46, "sys_ppu_thread_get_join_state", sys_ppu_thread_get_join_state},
    {47, "sys_ppu_thread_set_priority", sys_ppu_thread_set_priority},
    {48, "sys_ppu_thread_get_priority", sys_ppu_thread_get_priority},
    {52, "sys_ppu_thread_create", sys_ppu_thread_create},
    {53, "sys_ppu_thread_start", sys_ppu_thread_start},
    {56, "sys_ppu_thread_rename", sys_ppu_thread_rename},
};

} // namespace

std::span<const SyscallInfo> thread_syscalls() {
    return k_syscalls;
}

} // namespace lv2
