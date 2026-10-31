// Mutexes, condition variables and semaphores, through their syscalls.

#include "support/kernel_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace lv2;
using test::KernelTest;

namespace {

constexpr u64 k_data = KernelTest::k_data;
constexpr u64 k_attributes = k_data + 0x100;

// Syscall numbers. psdevwiki, "LV2 Functions and Syscalls".
constexpr u32 k_semaphore_create = 90, k_semaphore_destroy = 91, k_semaphore_wait = 92,
              k_semaphore_trywait = 93, k_semaphore_post = 94, k_semaphore_get_value = 114;
constexpr u32 k_mutex_create = 100, k_mutex_destroy = 101, k_mutex_lock = 102,
              k_mutex_trylock = 103, k_mutex_unlock = 104;
constexpr u32 k_cond_create = 105, k_cond_destroy = 106, k_cond_wait = 107, k_cond_signal = 108,
              k_cond_signal_all = 109, k_cond_signal_to = 110;

struct SyncTest : KernelTest {
    u32 create_mutex(PpuThread& thread, u32 protocol = k_protocol_fifo,
                     u32 recursive = k_mutex_not_recursive) {
        memory.write<u32>(k_attributes, protocol);
        memory.write<u32>(k_attributes + 4, recursive);
        syscall(thread, k_mutex_create, {k_data, k_attributes});
        REQUIRE(result(thread) == k_ok);
        return memory.read<u32>(k_data);
    }

    u32 create_cond(PpuThread& thread, u32 mutex) {
        syscall(thread, k_cond_create, {k_data, mutex, k_attributes});
        REQUIRE(result(thread) == k_ok);
        return memory.read<u32>(k_data);
    }

    u32 create_semaphore(PpuThread& thread, s32 initial, s32 max) {
        memory.write<u32>(k_attributes, k_protocol_fifo);
        syscall(thread, k_semaphore_create, {k_data, k_attributes, u64(initial), u64(max)});
        REQUIRE(result(thread) == k_ok);
        return memory.read<u32>(k_data);
    }

    Mutex& mutex(u32 id) { return *kernel.find_object<Mutex>(id); }
};

} // namespace

// ---- Mutex ----

TEST_CASE_METHOD(SyncTest, "a contended mutex goes to waiters in arrival order") {
    PpuThread& a = new_thread();
    PpuThread& b = new_thread();
    PpuThread& c = new_thread();
    const u32 id = create_mutex(a);

    syscall(a, k_mutex_lock, {id, 0});
    CHECK(result(a) == k_ok);
    syscall(b, k_mutex_lock, {id, 0});
    syscall(c, k_mutex_lock, {id, 0});
    CHECK(is_waiting(b));
    CHECK(is_waiting(c));

    syscall(a, k_mutex_unlock, {id});
    CHECK(mutex(id).owner == b.id);
    CHECK(b.status == ThreadStatus::ready);
    CHECK(result(b) == k_ok);
    CHECK(is_waiting(c));

    syscall(b, k_mutex_unlock, {id});
    CHECK(mutex(id).owner == c.id);
}

TEST_CASE_METHOD(SyncTest, "the priority protocol wakes the most urgent waiter") {
    PpuThread& owner = new_thread();
    PpuThread& relaxed = new_thread(2000);
    PpuThread& urgent = new_thread(100);
    const u32 id = create_mutex(owner, k_protocol_priority);

    syscall(owner, k_mutex_lock, {id, 0});
    syscall(relaxed, k_mutex_lock, {id, 0});
    syscall(urgent, k_mutex_lock, {id, 0});
    syscall(owner, k_mutex_unlock, {id});
    CHECK(mutex(id).owner == urgent.id);
}

TEST_CASE_METHOD(SyncTest, "recursive and non-recursive locking") {
    PpuThread& thread = new_thread();

    const u32 plain = create_mutex(thread);
    syscall(thread, k_mutex_lock, {plain, 0});
    syscall(thread, k_mutex_lock, {plain, 0});
    CHECK(result(thread) == k_error_deadlock);

    const u32 recursive = create_mutex(thread, k_protocol_fifo, k_mutex_recursive);
    syscall(thread, k_mutex_lock, {recursive, 0});
    syscall(thread, k_mutex_lock, {recursive, 0});
    CHECK(result(thread) == k_ok);
    syscall(thread, k_mutex_unlock, {recursive});
    CHECK(mutex(recursive).owner == thread.id); // still locked once
    syscall(thread, k_mutex_unlock, {recursive});
    CHECK(mutex(recursive).owner == 0);
}

TEST_CASE_METHOD(SyncTest, "mutex errors") {
    PpuThread& owner = new_thread();
    PpuThread& other = new_thread();
    const u32 id = create_mutex(owner);

    syscall(owner, k_mutex_lock, {id, 0});
    syscall(other, k_mutex_trylock, {id});
    CHECK(result(other) == k_error_busy);
    syscall(other, k_mutex_unlock, {id});
    CHECK(result(other) == k_error_not_permitted);
    syscall(other, k_mutex_destroy, {id});
    CHECK(result(other) == k_error_busy);
    syscall(other, k_mutex_lock, {0x9999, 0});
    CHECK(result(other) == k_error_no_such_id);

    memory.write<u32>(k_attributes, 7); // no such protocol
    syscall(owner, k_mutex_create, {k_data, k_attributes});
    CHECK(result(owner) == k_error_invalid);

    syscall(owner, k_mutex_unlock, {id});
    syscall(owner, k_mutex_destroy, {id});
    CHECK(result(owner) == k_ok);
}

TEST_CASE_METHOD(SyncTest, "a mutex wait can time out") {
    PpuThread& owner = new_thread();
    PpuThread& waiter = new_thread();
    const u32 id = create_mutex(owner);

    syscall(owner, k_mutex_lock, {id, 0});
    syscall(waiter, k_mutex_lock, {id, 10}); // 10 microseconds
    REQUIRE(kernel.skip_to_next_timeout());
    kernel.wake_timed_out_threads();
    CHECK(result(waiter) == k_error_timed_out);
    CHECK(mutex(id).owner == owner.id);
}

// ---- Condition variable ----

TEST_CASE_METHOD(SyncTest, "waiting releases the mutex; a signal hands it back on unlock") {
    PpuThread& waiter = new_thread();
    PpuThread& signaler = new_thread();
    const u32 mutex_id = create_mutex(waiter, k_protocol_fifo, k_mutex_recursive);
    const u32 cond_id = create_cond(waiter, mutex_id);

    syscall(waiter, k_mutex_lock, {mutex_id, 0});
    syscall(waiter, k_mutex_lock, {mutex_id, 0}); // locked twice
    syscall(waiter, k_cond_wait, {cond_id, 0});
    CHECK(is_waiting(waiter));
    CHECK(mutex(mutex_id).owner == 0);

    syscall(signaler, k_mutex_lock, {mutex_id, 0});
    syscall(signaler, k_cond_signal, {cond_id});
    CHECK(is_waiting(waiter)); // now waiting for the mutex instead

    syscall(signaler, k_mutex_unlock, {mutex_id});
    CHECK(waiter.status == ThreadStatus::ready);
    CHECK(result(waiter) == k_ok);
    CHECK(mutex(mutex_id).owner == waiter.id);
    CHECK(mutex(mutex_id).lock_count == 2); // restored
}

TEST_CASE_METHOD(SyncTest, "a timed-out condition wait still relocks the mutex") {
    PpuThread& waiter = new_thread();
    const u32 mutex_id = create_mutex(waiter);
    const u32 cond_id = create_cond(waiter, mutex_id);

    syscall(waiter, k_mutex_lock, {mutex_id, 0});
    syscall(waiter, k_cond_wait, {cond_id, 100});
    REQUIRE(kernel.skip_to_next_timeout());
    kernel.wake_timed_out_threads();
    CHECK(result(waiter) == k_error_timed_out);
    CHECK(mutex(mutex_id).owner == waiter.id);
}

TEST_CASE_METHOD(SyncTest, "signal_all and signal_to") {
    PpuThread& a = new_thread();
    PpuThread& b = new_thread();
    PpuThread& signaler = new_thread();
    const u32 mutex_id = create_mutex(a);
    const u32 cond_id = create_cond(a, mutex_id);

    for (PpuThread* thread : {&a, &b}) {
        syscall(*thread, k_mutex_lock, {mutex_id, 0});
        syscall(*thread, k_cond_wait, {cond_id, 0});
    }

    syscall(signaler, k_mutex_lock, {mutex_id, 0});
    syscall(signaler, k_cond_signal_to, {cond_id, b.id});
    CHECK(result(signaler) == k_ok);
    CHECK(kernel.first_waiter(WaitKind::mutex, mutex_id, false) == &b);

    syscall(signaler, k_cond_signal_all, {cond_id});
    CHECK_FALSE(kernel.has_waiters(WaitKind::cond, cond_id));
    syscall(signaler, k_mutex_unlock, {mutex_id});
    CHECK(mutex(mutex_id).owner == b.id); // b started waiting for the mutex first
}

TEST_CASE_METHOD(SyncTest, "condition variable errors") {
    PpuThread& owner = new_thread();
    PpuThread& other = new_thread();
    const u32 mutex_id = create_mutex(owner);
    const u32 cond_id = create_cond(owner, mutex_id);

    syscall(other, k_cond_wait, {cond_id, 0}); // without the mutex
    CHECK(result(other) == k_error_not_permitted);
    syscall(other, k_cond_signal, {cond_id});
    CHECK(result(other) == k_error_not_permitted);

    syscall(owner, k_mutex_destroy, {mutex_id}); // still bound to the condition variable
    CHECK(result(owner) == k_error_busy);
    syscall(owner, k_cond_destroy, {cond_id});
    CHECK(result(owner) == k_ok);
    syscall(owner, k_mutex_destroy, {mutex_id});
    CHECK(result(owner) == k_ok);
}

// ---- Semaphore ----

TEST_CASE_METHOD(SyncTest, "semaphores count down and wake waiters on post") {
    PpuThread& a = new_thread();
    PpuThread& b = new_thread();
    const u32 id = create_semaphore(a, 1, 2);

    syscall(a, k_semaphore_wait, {id, 0});
    CHECK(result(a) == k_ok);
    syscall(b, k_semaphore_trywait, {id});
    CHECK(result(b) == k_error_busy);
    syscall(b, k_semaphore_wait, {id, 0});
    CHECK(is_waiting(b));

    syscall(a, k_semaphore_post, {id, 1});
    CHECK(b.status == ThreadStatus::ready);
    syscall(a, k_semaphore_get_value, {id, k_data});
    CHECK(memory.read<u32>(k_data) == 0); // the post went straight to b
}

TEST_CASE_METHOD(SyncTest, "semaphore errors") {
    PpuThread& thread = new_thread();
    const u32 id = create_semaphore(thread, 2, 2);

    syscall(thread, k_semaphore_post, {id, 1});
    CHECK(result(thread) == k_error_busy); // would exceed the maximum
    syscall(thread, k_semaphore_post, {id, 0});
    CHECK(result(thread) == k_error_invalid);

    memory.write<u32>(k_attributes, k_protocol_fifo);
    syscall(thread, k_semaphore_create, {k_data, k_attributes, 3, 2}); // initial > max
    CHECK(result(thread) == k_error_invalid);

    syscall(thread, k_semaphore_destroy, {id});
    CHECK(result(thread) == k_ok);
    syscall(thread, k_semaphore_wait, {id, 0});
    CHECK(result(thread) == k_error_no_such_id);
}
