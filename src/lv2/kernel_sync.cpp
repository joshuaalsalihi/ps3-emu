// Mutexes, condition variables and semaphores. The syscalls and the HLE lightweight versions
// (sys_lwmutex, sys_lwcond) share these.

#include "lv2/kernel.h"

namespace lv2 {

// ---- Mutex ----

std::optional<s32> Kernel::lock_mutex(PpuThread& thread, u32 mutex_id, u64 timeout_us) {
    Mutex* mutex = find_object<Mutex>(mutex_id);
    if (!mutex) return k_error_no_such_id;

    if (mutex->owner == 0) {
        mutex->owner = thread.id;
        mutex->lock_count = 1;
        return k_ok;
    }
    if (mutex->owner == thread.id) {
        if (!mutex->recursive) return k_error_deadlock;
        ++mutex->lock_count;
        return k_ok;
    }
    wait(thread, Wait::on(WaitKind::mutex, mutex_id), timeout_us);
    return std::nullopt;
}

s32 Kernel::try_lock_mutex(PpuThread& thread, u32 mutex_id) {
    Mutex* mutex = find_object<Mutex>(mutex_id);
    if (!mutex) return k_error_no_such_id;
    if (mutex->owner != 0 && mutex->owner != thread.id) return k_error_busy;
    return *lock_mutex(thread, mutex_id, 0); // can't wait: the mutex is free or ours
}

s32 Kernel::unlock_mutex(PpuThread& thread, u32 mutex_id) {
    Mutex* mutex = find_object<Mutex>(mutex_id);
    if (!mutex) return k_error_no_such_id;
    if (mutex->owner != thread.id) return k_error_not_permitted;

    if (--mutex->lock_count == 0) hand_off_mutex(*mutex, mutex_id);
    return k_ok;
}

// Gives an unlocked mutex to its next waiter, if any.
void Kernel::hand_off_mutex(Mutex& mutex, u32 mutex_id) {
    PpuThread* next = first_waiter(WaitKind::mutex, mutex_id, mutex.wake_by_priority);
    if (!next) {
        mutex.owner = 0;
        mutex.lock_count = 0;
        return;
    }
    mutex.owner = next->id;
    mutex.lock_count = std::max<u32>(next->wait.saved_lock_count, 1);
    wake(*next, next->wait.result);
}

s32 Kernel::destroy_mutex(u32 mutex_id) {
    Mutex* mutex = find_object<Mutex>(mutex_id);
    if (!mutex) return k_error_no_such_id;
    if (mutex->owner != 0 || mutex->condition_variables != 0 ||
        has_waiters(WaitKind::mutex, mutex_id)) {
        return k_error_busy;
    }
    remove_object(mutex_id);
    return k_ok;
}

// ---- Condition variable ----

s32 Kernel::create_cond(u32 mutex_id, u32& cond_id) {
    Mutex* mutex = find_object<Mutex>(mutex_id);
    if (!mutex) return k_error_no_such_id;

    auto cond = std::make_unique<Cond>();
    cond->mutex = mutex_id;
    ++mutex->condition_variables;
    cond_id = add_object(std::move(cond));
    return k_ok;
}

std::optional<s32> Kernel::wait_cond(PpuThread& thread, u32 cond_id, u64 timeout_us) {
    Cond* cond = find_object<Cond>(cond_id);
    if (!cond) return k_error_no_such_id;
    Mutex* mutex = find_object<Mutex>(cond->mutex);
    if (mutex->owner != thread.id) return k_error_not_permitted;

    // Release the mutex completely while waiting, and remember how deeply it was locked.
    const u32 saved_lock_count = mutex->lock_count;
    hand_off_mutex(*mutex, cond->mutex);

    Wait cond_wait = Wait::on(WaitKind::cond, cond_id);
    cond_wait.saved_lock_count = saved_lock_count;
    wait(thread, cond_wait, timeout_us);
    return std::nullopt;
}

// A condition variable wait is over (signaled or timed out). The thread must lock the mutex
// again before it returns, so it either takes the mutex now or waits for it.
void Kernel::finish_cond_wait(PpuThread& waiter, s32 result) {
    const u32 mutex_id = find_object<Cond>(waiter.wait.object)->mutex;
    Mutex* mutex = find_object<Mutex>(mutex_id);

    if (mutex->owner == 0) {
        mutex->owner = waiter.id;
        mutex->lock_count = waiter.wait.saved_lock_count;
        wake(waiter, result);
        return;
    }
    Wait mutex_wait = Wait::on(WaitKind::mutex, mutex_id);
    mutex_wait.result = result;
    mutex_wait.saved_lock_count = waiter.wait.saved_lock_count;
    wait(waiter, mutex_wait, 0);
}

s32 Kernel::signal_cond(PpuThread& thread, u32 cond_id) {
    Cond* cond = find_object<Cond>(cond_id);
    if (!cond) return k_error_no_such_id;
    if (find_object<Mutex>(cond->mutex)->owner != thread.id) return k_error_not_permitted;

    const bool by_priority = find_object<Mutex>(cond->mutex)->wake_by_priority;
    if (PpuThread* waiter = first_waiter(WaitKind::cond, cond_id, by_priority)) {
        finish_cond_wait(*waiter, k_ok);
    }
    return k_ok;
}

s32 Kernel::signal_cond_all(PpuThread& thread, u32 cond_id) {
    Cond* cond = find_object<Cond>(cond_id);
    if (!cond) return k_error_no_such_id;
    if (find_object<Mutex>(cond->mutex)->owner != thread.id) return k_error_not_permitted;

    while (PpuThread* waiter = first_waiter(WaitKind::cond, cond_id, false)) {
        finish_cond_wait(*waiter, k_ok);
    }
    return k_ok;
}

s32 Kernel::signal_cond_to(PpuThread& thread, u32 cond_id, u32 thread_id) {
    Cond* cond = find_object<Cond>(cond_id);
    if (!cond) return k_error_no_such_id;
    if (find_object<Mutex>(cond->mutex)->owner != thread.id) return k_error_not_permitted;

    PpuThread* target = find_thread(thread_id);
    if (!target) return k_error_no_such_id;
    // UNVERIFIED: the error for a thread that exists but isn't waiting here.
    if (target->status != ThreadStatus::waiting || target->wait.kind != WaitKind::cond ||
        target->wait.object != cond_id) {
        return k_error_not_permitted;
    }
    finish_cond_wait(*target, k_ok);
    return k_ok;
}

s32 Kernel::destroy_cond(u32 cond_id) {
    Cond* cond = find_object<Cond>(cond_id);
    if (!cond) return k_error_no_such_id;
    if (has_waiters(WaitKind::cond, cond_id)) return k_error_busy;

    --find_object<Mutex>(cond->mutex)->condition_variables;
    remove_object(cond_id);
    return k_ok;
}

// ---- Semaphore ----

std::optional<s32> Kernel::wait_semaphore(PpuThread& thread, u32 semaphore_id, u64 timeout_us) {
    Semaphore* semaphore = find_object<Semaphore>(semaphore_id);
    if (!semaphore) return k_error_no_such_id;

    if (semaphore->value > 0) {
        --semaphore->value;
        return k_ok;
    }
    wait(thread, Wait::on(WaitKind::semaphore, semaphore_id), timeout_us);
    return std::nullopt;
}

s32 Kernel::try_wait_semaphore(u32 semaphore_id) {
    Semaphore* semaphore = find_object<Semaphore>(semaphore_id);
    if (!semaphore) return k_error_no_such_id;
    if (semaphore->value <= 0) return k_error_busy;
    --semaphore->value;
    return k_ok;
}

s32 Kernel::post_semaphore(u32 semaphore_id, s32 count) {
    Semaphore* semaphore = find_object<Semaphore>(semaphore_id);
    if (!semaphore) return k_error_no_such_id;
    if (count <= 0) return k_error_invalid;
    if (count > semaphore->max - semaphore->value) return k_error_busy;

    semaphore->value += count;
    while (semaphore->value > 0) {
        PpuThread* waiter =
            first_waiter(WaitKind::semaphore, semaphore_id, semaphore->wake_by_priority);
        if (!waiter) break;
        --semaphore->value;
        wake(*waiter, k_ok);
    }
    return k_ok;
}

s32 Kernel::destroy_semaphore(u32 semaphore_id) {
    if (!find_object<Semaphore>(semaphore_id)) return k_error_no_such_id;
    if (has_waiters(WaitKind::semaphore, semaphore_id)) return k_error_busy;
    remove_object(semaphore_id);
    return k_ok;
}

} // namespace lv2
