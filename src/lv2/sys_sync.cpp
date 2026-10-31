// Mutex, condition variable and semaphore syscalls. The logic is in kernel_sync.cpp; these
// read the arguments and write the results.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

u32 id_arg(const PpuThread& thread, u32 index) {
    return static_cast<u32>(thread.arg(index));
}

// Sets the result, unless the thread is now waiting (it gets its result when it wakes).
void finish(PpuThread& thread, std::optional<s32> result) {
    if (result) thread.set_result(*result);
}

// Output pointers are checked before an object is created, so a bad one doesn't leak it.
bool can_write(Kernel& kernel, u32 address, u64 size) {
    return kernel.memory.is_accessible(address, size, Memory::k_write);
}

// ---- Mutex ----

// int sys_mutex_create(sys_mutex_t* mutex_id, sys_mutex_attribute_t* attr)
// The attribute starts {u32 protocol, u32 recursive, ...}. PSL1GHT, sys/mutex.h.
void sys_mutex_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const u32 protocol = kernel.memory.read<u32>(thread.pointer_arg(1));
    const u32 recursive = kernel.memory.read<u32>(thread.pointer_arg(1) + 4);

    if (protocol < k_protocol_fifo || protocol > k_protocol_priority_inherit ||
        (recursive != k_mutex_recursive && recursive != k_mutex_not_recursive)) {
        return thread.set_result(k_error_invalid);
    }
    if (!can_write(kernel, id_out, 4)) return thread.set_result(k_error_bad_address);

    auto mutex = std::make_unique<Mutex>();
    mutex->recursive = recursive == k_mutex_recursive;
    mutex->wake_by_priority = protocol != k_protocol_fifo;
    kernel.memory.write<u32>(id_out, kernel.add_object(std::move(mutex)));
    thread.set_result(k_ok);
}

// int sys_mutex_destroy(sys_mutex_t mutex_id)
void sys_mutex_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_mutex(id_arg(thread, 0)));
}

// int sys_mutex_lock(sys_mutex_t mutex_id, usecond_t timeout)
void sys_mutex_lock(Kernel& kernel, PpuThread& thread) {
    finish(thread, kernel.lock_mutex(thread, id_arg(thread, 0), thread.arg(1)));
}

// int sys_mutex_trylock(sys_mutex_t mutex_id)
void sys_mutex_trylock(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.try_lock_mutex(thread, id_arg(thread, 0)));
}

// int sys_mutex_unlock(sys_mutex_t mutex_id)
void sys_mutex_unlock(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.unlock_mutex(thread, id_arg(thread, 0)));
}

// ---- Condition variable ----

// int sys_cond_create(sys_cond_t* cond_id, sys_mutex_t mutex_id, sys_cond_attribute_t* attr)
void sys_cond_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    if (!can_write(kernel, id_out, 4)) return thread.set_result(k_error_bad_address);

    u32 cond_id = 0;
    const s32 result = kernel.create_cond(id_arg(thread, 1), cond_id);
    if (result == k_ok) kernel.memory.write<u32>(id_out, cond_id);
    thread.set_result(result);
}

// int sys_cond_destroy(sys_cond_t cond_id)
void sys_cond_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_cond(id_arg(thread, 0)));
}

// int sys_cond_wait(sys_cond_t cond_id, usecond_t timeout)
void sys_cond_wait(Kernel& kernel, PpuThread& thread) {
    finish(thread, kernel.wait_cond(thread, id_arg(thread, 0), thread.arg(1)));
}

// int sys_cond_signal(sys_cond_t cond_id)
void sys_cond_signal(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.signal_cond(thread, id_arg(thread, 0)));
}

// int sys_cond_signal_all(sys_cond_t cond_id)
void sys_cond_signal_all(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.signal_cond_all(thread, id_arg(thread, 0)));
}

// int sys_cond_signal_to(sys_cond_t cond_id, sys_ppu_thread_t thread_id)
void sys_cond_signal_to(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.signal_cond_to(thread, id_arg(thread, 0), id_arg(thread, 1)));
}

// ---- Semaphore ----

// int sys_semaphore_create(sys_semaphore_t* sem, sys_semaphore_attribute_t* attr,
//                          int initial_value, int max_value)
// The attribute starts {u32 protocol, ...}. PSL1GHT, sys/sem.h.
void sys_semaphore_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const u32 protocol = kernel.memory.read<u32>(thread.pointer_arg(1));
    const s32 initial = static_cast<s32>(thread.arg(2));
    const s32 max = static_cast<s32>(thread.arg(3));

    if (protocol < k_protocol_fifo || protocol > k_protocol_priority_inherit || max <= 0 ||
        initial < 0 || initial > max) {
        return thread.set_result(k_error_invalid);
    }
    if (!can_write(kernel, id_out, 4)) return thread.set_result(k_error_bad_address);

    auto semaphore = std::make_unique<Semaphore>();
    semaphore->value = initial;
    semaphore->max = max;
    semaphore->wake_by_priority = protocol != k_protocol_fifo;
    kernel.memory.write<u32>(id_out, kernel.add_object(std::move(semaphore)));
    thread.set_result(k_ok);
}

// int sys_semaphore_destroy(sys_semaphore_t sem)
void sys_semaphore_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_semaphore(id_arg(thread, 0)));
}

// int sys_semaphore_wait(sys_semaphore_t sem, usecond_t timeout)
void sys_semaphore_wait(Kernel& kernel, PpuThread& thread) {
    finish(thread, kernel.wait_semaphore(thread, id_arg(thread, 0), thread.arg(1)));
}

// int sys_semaphore_trywait(sys_semaphore_t sem)
void sys_semaphore_trywait(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.try_wait_semaphore(id_arg(thread, 0)));
}

// int sys_semaphore_post(sys_semaphore_t sem, int count)
void sys_semaphore_post(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.post_semaphore(id_arg(thread, 0), static_cast<s32>(thread.arg(1))));
}

// int sys_semaphore_get_value(sys_semaphore_t sem, int* value)
void sys_semaphore_get_value(Kernel& kernel, PpuThread& thread) {
    const Semaphore* semaphore = kernel.find_object<Semaphore>(id_arg(thread, 0));
    if (!semaphore) return thread.set_result(k_error_no_such_id);

    kernel.memory.write<u32>(thread.pointer_arg(1), static_cast<u32>(semaphore->value));
    thread.set_result(k_ok);
}

constexpr SyscallInfo k_syscalls[] = {
    {90, "sys_semaphore_create", sys_semaphore_create},
    {91, "sys_semaphore_destroy", sys_semaphore_destroy},
    {92, "sys_semaphore_wait", sys_semaphore_wait},
    {93, "sys_semaphore_trywait", sys_semaphore_trywait},
    {94, "sys_semaphore_post", sys_semaphore_post},
    {100, "sys_mutex_create", sys_mutex_create},
    {101, "sys_mutex_destroy", sys_mutex_destroy},
    {102, "sys_mutex_lock", sys_mutex_lock},
    {103, "sys_mutex_trylock", sys_mutex_trylock},
    {104, "sys_mutex_unlock", sys_mutex_unlock},
    {105, "sys_cond_create", sys_cond_create},
    {106, "sys_cond_destroy", sys_cond_destroy},
    {107, "sys_cond_wait", sys_cond_wait},
    {108, "sys_cond_signal", sys_cond_signal},
    {109, "sys_cond_signal_all", sys_cond_signal_all},
    {110, "sys_cond_signal_to", sys_cond_signal_to},
    {114, "sys_semaphore_get_value", sys_semaphore_get_value},
};

} // namespace

std::span<const SyscallInfo> sync_syscalls() {
    return k_syscalls;
}

} // namespace lv2
