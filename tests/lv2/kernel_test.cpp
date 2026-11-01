// Kernel core: syscall dispatch, threads, scheduling and timeouts.

#include "lv2/kernel.h"

#include "support/kernel_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace lv2;
using test::KernelTest;

namespace {

constexpr u64 k_cycles_per_microsecond = 3200;

} // namespace

// ---- Syscall dispatch ----

TEST_CASE_METHOD(KernelTest, "unimplemented syscalls return ENOSYS") {
    PpuThread& thread = new_thread();
    syscall(thread, 999);
    CHECK(thread.cpu.state.gpr[3] == 0xffff'ffff'8001'0003); // sign-extended
}

TEST_CASE_METHOD(KernelTest, "a bad pointer argument returns EFAULT") {
    PpuThread& thread = new_thread();
    syscall(thread, 46, {0x9000'0000}); // sys_ppu_thread_get_join_state(int*)
    CHECK(result(thread) == k_error_bad_address);
}

TEST_CASE_METHOD(KernelTest, "process syscalls") {
    PpuThread& thread = new_thread();
    syscall(thread, 147); // sys_time_get_timebase_frequency
    CHECK(thread.cpu.state.gpr[3] == 79'800'000);

    CHECK_FALSE(kernel.exit_status());
    syscall(thread, 3, {42}); // sys_process_exit
    CHECK(kernel.exit_status() == 42);
}

// ---- Console ----

TEST_CASE_METHOD(KernelTest, "sys_tty_write prints guest memory and reports its length") {
    PpuThread& thread = new_thread();
    write_string(k_data, "hello");
    syscall(thread, 403, {0, k_data, 5, k_data + 0x100});
    CHECK(tty.str() == "hello");
    CHECK(result(thread) == k_ok);
    CHECK(memory.read<u32>(k_data + 0x100) == 5);
}

TEST_CASE_METHOD(KernelTest, "sys_tty_write rejects bad buffers before allocating") {
    PpuThread& thread = new_thread();
    syscall(thread, 403, {0, 0x9000'0000, 5, 0});
    CHECK(result(thread) == k_error_bad_address);

    syscall(thread, 403, {0, k_data, 0xffff'ffff, 0}); // runs past the end of memory
    CHECK(result(thread) == k_error_bad_address);
    CHECK(tty.str().empty());
}

TEST_CASE_METHOD(KernelTest, "sys_fs_fstat reports that no file is open") {
    PpuThread& thread = new_thread();
    syscall(thread, 809, {1, k_data});
    CHECK(result(thread) == k_error_bad_file_descriptor);
}

// ---- Thread creation ----

TEST_CASE_METHOD(KernelTest, "new threads get a stack, a guard page and their arguments") {
    PpuThread* thread = kernel.create_thread("worker", 0x12340, 0x28000, 77, 500, 0x5000);
    REQUIRE(thread != nullptr);
    const ppu::State& state = thread->cpu.state;

    CHECK(thread->status == ThreadStatus::stopped);
    CHECK(thread->stack_size == 0x5000);
    CHECK(state.pc == 0x12340);
    CHECK(state.gpr[2] == 0x28000);
    CHECK(state.gpr[3] == 77);
    CHECK(state.lr == Kernel::k_thread_exit_stub);
    CHECK(state.gpr[1] > thread->stack_address);
    CHECK(state.gpr[1] < thread->stack_address + thread->stack_size);
    CHECK(memory.is_accessible(thread->stack_address, thread->stack_size, Memory::k_read_write));
    CHECK_FALSE(memory.is_accessible(thread->stack_address - 1, 1, Memory::k_read));
}

TEST_CASE_METHOD(KernelTest, "a thread whose stack can't fit is refused") {
    CHECK(kernel.create_thread("huge", 0x10000, 0, 0, 1000, ~u64{0}) == nullptr);
    CHECK(kernel.create_thread("big", 0x10000, 0, 0, 1000, Kernel::k_stack_area_size) == nullptr);
}

TEST_CASE_METHOD(KernelTest, "each thread gets its own copy of the TLS template") {
    memory.write<u32>(k_data, 0xcafef00d); // 4 initialized bytes, then 12 zeroed ones
    kernel.set_tls_template(k_data, 4, 16);

    PpuThread& first = new_thread();
    PpuThread& second = new_thread();
    // The ABI's thread pointer is 0x7000 past the start of the TLS data.
    const u64 first_tls = first.cpu.state.gpr[13] - 0x7000;
    const u64 second_tls = second.cpu.state.gpr[13] - 0x7000;

    CHECK(first_tls != second_tls);
    CHECK(memory.read<u32>(first_tls) == 0xcafef00d);
    CHECK(memory.read<u32>(second_tls) == 0xcafef00d);
    CHECK(memory.read<u32>(first_tls + 12) == 0);
}

TEST_CASE_METHOD(KernelTest, "the thread exit stub calls sys_ppu_thread_exit") {
    CHECK(memory.read<u32>(Kernel::k_thread_exit_stub) == 0x39600029);     // li r11, 41
    CHECK(memory.read<u32>(Kernel::k_thread_exit_stub + 4) == 0x44000002); // sc
}

// ---- Scheduling ----

TEST_CASE_METHOD(KernelTest, "ready threads take turns in creation order") {
    PpuThread& a = new_thread();
    PpuThread& b = new_thread();
    PpuThread* stopped = kernel.create_thread("stopped", 0x10000, 0, 0, 1000, 0x4000);
    PpuThread& c = new_thread();
    kernel.wait(b, Wait::on(WaitKind::sleep), 0);

    CHECK(kernel.next_thread().ppu == &a);
    CHECK(kernel.next_thread().ppu == &c); // b waits; the stopped thread never runs
    CHECK(kernel.next_thread().ppu == &a);
    CHECK(stopped->status == ThreadStatus::stopped);
}

TEST_CASE_METHOD(KernelTest, "no thread is ready when all are waiting") {
    PpuThread& thread = new_thread();
    kernel.wait(thread, Wait::on(WaitKind::sleep), 0);
    CHECK_FALSE(kernel.next_thread());
    CHECK_FALSE(kernel.all_threads_finished());
    CHECK_FALSE(kernel.skip_to_next_timeout());
}

TEST_CASE_METHOD(KernelTest, "waiters are woken in arrival order, or by priority") {
    PpuThread& late_urgent = new_thread(100);
    PpuThread& early_relaxed = new_thread(2000);
    kernel.wait(early_relaxed, Wait::on(WaitKind::mutex, 5), 0);
    kernel.wait(late_urgent, Wait::on(WaitKind::mutex, 5), 0);

    CHECK(kernel.first_waiter(WaitKind::mutex, 5, false) == &early_relaxed);
    CHECK(kernel.first_waiter(WaitKind::mutex, 5, true) == &late_urgent);
    CHECK(kernel.first_waiter(WaitKind::mutex, 6, false) == nullptr);
}

// ---- Sleeping and timeouts ----

TEST_CASE_METHOD(KernelTest, "usleep waits on the virtual clock") {
    PpuThread& thread = new_thread();
    syscall(thread, 141, {250}); // sys_timer_usleep
    CHECK(is_waiting(thread));

    // Nothing else can run, so the clock jumps straight to the wake-up time.
    REQUIRE(kernel.skip_to_next_timeout());
    CHECK(clock.cycles() == 250 * k_cycles_per_microsecond);
    kernel.wake_timed_out_threads();
    CHECK(thread.status == ThreadStatus::ready);
    CHECK(result(thread) == k_ok);
}

TEST_CASE_METHOD(KernelTest, "a sleep of zero returns at once") {
    PpuThread& thread = new_thread();
    syscall(thread, 141, {0});
    CHECK(thread.status == ThreadStatus::ready);
}

TEST_CASE_METHOD(KernelTest, "sys_time_get_current_time counts from a fixed boot time") {
    PpuThread& thread = new_thread();
    clock.advance(3'200'000'000 + 3200); // one second and one microsecond
    syscall(thread, 145, {k_data, k_data + 8});
    CHECK(memory.read<u64>(k_data) == Kernel::k_boot_time_seconds + 1);
    CHECK(memory.read<u64>(k_data + 8) == 1000);
}

// ---- Thread syscalls ----

TEST_CASE_METHOD(KernelTest, "sys_ppu_thread_create makes a stopped thread from a descriptor") {
    PpuThread& creator = new_thread();
    // Descriptor {code, TOC} at k_data; the parameter block {descriptor, tls} at k_data + 16.
    memory.write<u32>(k_data, 0x12340);
    memory.write<u32>(k_data + 4, 0x28000);
    memory.write<u32>(k_data + 16, k_data);
    write_string(k_data + 32, "worker");

    syscall(creator, 52, {k_data + 64, k_data + 16, 99, 0, 500, 0x8000, 1, k_data + 32});
    REQUIRE(result(creator) == k_ok);

    PpuThread* worker = kernel.find_thread(static_cast<u32>(memory.read<u64>(k_data + 64)));
    REQUIRE(worker != nullptr);
    CHECK(worker->name == "worker");
    CHECK(worker->status == ThreadStatus::stopped);
    CHECK(worker->cpu.state.pc == 0x12340);
    CHECK(worker->cpu.state.gpr[2] == 0x28000);
    CHECK(worker->cpu.state.gpr[3] == 99);
    CHECK(worker->joinable);

    syscall(creator, 53, {worker->id}); // sys_ppu_thread_start
    CHECK(result(creator) == k_ok);
    CHECK(worker->status == ThreadStatus::ready);

    syscall(creator, 53, {worker->id});
    CHECK(result(creator) == k_error_bad_state);
}

TEST_CASE_METHOD(KernelTest, "sys_ppu_thread_create checks the priority") {
    PpuThread& creator = new_thread();
    memory.write<u32>(k_data + 16, k_data);
    syscall(creator, 52, {k_data + 64, k_data + 16, 0, 0, 5000, 0x4000, 0, 0});
    CHECK(result(creator) == k_error_invalid);
}

TEST_CASE_METHOD(KernelTest, "join waits for the thread's exit value") {
    PpuThread& main = new_thread();
    PpuThread& worker = new_thread();

    syscall(main, 44, {worker.id, k_data}); // sys_ppu_thread_join
    CHECK(is_waiting(main));

    syscall(worker, 41, {1234}); // sys_ppu_thread_exit
    CHECK(worker.status == ThreadStatus::finished);
    CHECK(main.status == ThreadStatus::ready);
    CHECK(result(main) == k_ok);
    CHECK(memory.read<u64>(k_data) == 1234);
}

TEST_CASE_METHOD(KernelTest, "joining a finished thread returns at once, but only once") {
    PpuThread& main = new_thread();
    PpuThread& worker = new_thread();
    syscall(worker, 41, {7});

    syscall(main, 44, {worker.id, k_data});
    CHECK(result(main) == k_ok);
    CHECK(memory.read<u64>(k_data) == 7);

    syscall(main, 44, {worker.id, k_data});
    CHECK(result(main) == k_error_invalid);
}

TEST_CASE_METHOD(KernelTest, "join errors") {
    PpuThread& main = new_thread();
    PpuThread& worker = new_thread();

    syscall(main, 44, {main.id, 0});
    CHECK(result(main) == k_error_deadlock);
    syscall(main, 44, {0x9999, 0});
    CHECK(result(main) == k_error_no_such_id);

    syscall(main, 45, {worker.id}); // sys_ppu_thread_detach
    CHECK(result(main) == k_ok);
    syscall(main, 44, {worker.id, 0});
    CHECK(result(main) == k_error_invalid);
}

TEST_CASE_METHOD(KernelTest, "priority, join state and rename") {
    PpuThread& thread = new_thread();

    syscall(thread, 47, {thread.id, 1234});   // set priority
    syscall(thread, 48, {thread.id, k_data}); // get priority
    CHECK(memory.read<u32>(k_data) == 1234);

    syscall(thread, 46, {k_data}); // get join state
    CHECK(memory.read<u32>(k_data) == 1);

    write_string(k_data + 16, "renamed");
    syscall(thread, 56, {thread.id, k_data + 16});
    CHECK(thread.name == "renamed");
}

TEST_CASE_METHOD(KernelTest, "the process is over when every thread has finished") {
    PpuThread& thread = new_thread();
    CHECK_FALSE(kernel.all_threads_finished());
    syscall(thread, 41, {0});
    CHECK(kernel.all_threads_finished());
}
