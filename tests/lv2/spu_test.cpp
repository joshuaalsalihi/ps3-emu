// SPU thread groups and SPU threads, driven through their syscalls.

#include "support/kernel_test.h"

#include "cpu/spu/assembler.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace lv2;
using test::KernelTest;

namespace {

// Syscall numbers. psdevwiki, "LV2 Functions and Syscalls".
constexpr u32 k_get_exit_status = 165;
constexpr u32 k_set_argument = 166;
constexpr u32 k_group_create = 170;
constexpr u32 k_group_destroy = 171;
constexpr u32 k_thread_initialize = 172;
constexpr u32 k_group_start = 173;
constexpr u32 k_group_terminate = 177;
constexpr u32 k_group_join = 178;
constexpr u32 k_write_ls = 181;
constexpr u32 k_read_ls = 182;
constexpr u32 k_write_snr = 184;
constexpr u32 k_set_spu_cfg = 187;
constexpr u32 k_write_spu_mb = 190;
constexpr u32 k_connect_event = 191;
constexpr u32 k_disconnect_event = 192;
constexpr u32 k_bind_queue = 193;
constexpr u32 k_unbind_queue = 194;
constexpr u32 k_queue_create = 128;
constexpr u32 k_queue_receive = 130;
constexpr u32 k_port_create = 134;
constexpr u32 k_port_connect = 136;
constexpr u32 k_port_send = 138;

// Where the test puts things in the data page.
constexpr u64 k_code = KernelTest::k_data;
constexpr u64 k_segments = KernelTest::k_data + 0x400;
constexpr u64 k_image = KernelTest::k_data + 0x420;
constexpr u64 k_thread_attr = KernelTest::k_data + 0x440;
constexpr u64 k_group_attr = KernelTest::k_data + 0x460;
constexpr u64 k_name = KernelTest::k_data + 0x480;
constexpr u64 k_arguments = KernelTest::k_data + 0x4c0;
constexpr u64 k_group_id = KernelTest::k_data + 0x500;
constexpr u64 k_thread_id = KernelTest::k_data + 0x504;
constexpr u64 k_cause = KernelTest::k_data + 0x508;
constexpr u64 k_status = KernelTest::k_data + 0x50c;
constexpr u64 k_value = KernelTest::k_data + 0x510;
constexpr u64 k_queue_attr = KernelTest::k_data + 0x520;
constexpr u64 k_queue_id = KernelTest::k_data + 0x540;
constexpr u64 k_port_id = KernelTest::k_data + 0x544;

class SpuKernelTest : public KernelTest {
public:
    SpuKernelTest() {
        write_string(k_name, "spu");
        memory.write<u32>(k_thread_attr, static_cast<u32>(k_name)); // name, size, option
        memory.write<u32>(k_thread_attr + 4, 4);
        memory.write<u32>(k_group_attr, 4); // size, name, type, container
        memory.write<u32>(k_group_attr + 4, static_cast<u32>(k_name));
    }

    // Writes an SPU program and an image that copies it to local store address 0.
    void write_program(std::initializer_list<std::string_view> program) {
        write_program(std::vector<std::string_view>(program));
    }
    void write_program(const std::vector<std::string_view>& lines) {
        const std::vector<u32> words = spu::assemble(lines, 0);
        for (std::size_t i = 0; i < words.size(); ++i) {
            memory.write<u32>(k_code + 4 * i, words[i]);
        }
        memory.write<u32>(k_segments, k_spu_segment_copy);
        memory.write<u32>(k_segments + 4, 0);
        memory.write<u32>(k_segments + 8, static_cast<u32>(4 * words.size()));
        memory.write<u32>(k_segments + 12, static_cast<u32>(k_code));
        memory.write<u32>(k_image + 4, 0); // entry point
        memory.write<u32>(k_image + 8, static_cast<u32>(k_segments));
        memory.write<u32>(k_image + 12, 1);
    }

    // Creates a group of `size` threads running the program, all initialized.
    u32 create_group(u32 size = 1) {
        syscall(ppu, k_group_create, {k_group_id, size, 100, k_group_attr});
        REQUIRE(result(ppu) == k_ok);
        const u32 group = memory.read<u32>(k_group_id);
        for (u32 i = 0; i < size; ++i) {
            syscall(ppu, k_thread_initialize,
                    {k_thread_id, group, i, k_image, k_thread_attr, k_arguments});
            REQUIRE(result(ppu) == k_ok);
            threads.push_back(memory.read<u32>(k_thread_id));
        }
        return group;
    }

    SpuThread& spu_thread(std::size_t i = 0) { return *kernel.find_spu_thread(threads.at(i)); }

    // Gives an SPU thread a turn, as System::run does.
    spu::SpeRun run_spu(SpuThread& thread) {
        const spu::SpeRun run = spu::run(*thread.spe, 10'000);
        if (run.exit == spu::SpeExit::stalled) kernel.block_spu_thread(thread, run.channel);
        if (run.exit == spu::SpeExit::stopped) kernel.handle_spu_stop(thread, run.signal);
        if (run.exit == spu::SpeExit::interrupt) kernel.handle_spu_interrupt(thread);
        return run;
    }

    u32 create_queue(u32 type) {
        memory.write<u32>(k_queue_attr, k_protocol_fifo);
        memory.write<u32>(k_queue_attr + 4, type);
        syscall(ppu, k_queue_create, {k_queue_id, k_queue_attr, 0, 8});
        REQUIRE(result(ppu) == k_ok);
        return memory.read<u32>(k_queue_id);
    }

    // A PPU port connected to `queue`, for sending it events.
    u32 create_port(u32 queue) {
        syscall(ppu, k_port_create, {k_port_id, k_event_port_local, 0x99});
        const u32 port = memory.read<u32>(k_port_id);
        syscall(ppu, k_port_connect, {port, queue});
        REQUIRE(result(ppu) == k_ok);
        return port;
    }

    // The exit status the thread passed to spu_thread_exit.
    std::optional<s32> exit_status(std::size_t i = 0) { return spu_thread(i).exit_status; }

    PpuThread& ppu = new_thread();
    std::vector<u32> threads;
};

} // namespace

TEST_CASE_METHOD(SpuKernelTest, "group creation checks its size and priority") {
    syscall(ppu, k_group_create, {k_group_id, 0, 100, k_group_attr});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_group_create, {k_group_id, 7, 100, k_group_attr});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_group_create, {k_group_id, 1, 5, k_group_attr});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_group_create, {k_group_id, 6, 255, k_group_attr});
    CHECK(result(ppu) == k_ok);
    const auto* group = kernel.find_object<SpuThreadGroup>(memory.read<u32>(k_group_id));
    REQUIRE(group);
    CHECK(group->name == "spu");
}

TEST_CASE_METHOD(SpuKernelTest, "a group starts only when all its threads are initialized") {
    write_program({"stop 0x102"});
    syscall(ppu, k_group_create, {k_group_id, 2, 100, k_group_attr});
    const u32 group = memory.read<u32>(k_group_id);
    syscall(ppu, k_thread_initialize, {k_thread_id, group, 0, k_image, k_thread_attr, 0});
    REQUIRE(result(ppu) == k_ok);
    syscall(ppu, k_group_start, {group});
    CHECK(result(ppu) == k_error_bad_state);

    syscall(ppu, k_thread_initialize, {k_thread_id, group, 0, k_image, k_thread_attr, 0});
    CHECK(result(ppu) == k_error_busy); // index 0 is taken
    syscall(ppu, k_thread_initialize, {k_thread_id, group, 2, k_image, k_thread_attr, 0});
    CHECK(result(ppu) == k_error_invalid); // out of range
    syscall(ppu, k_thread_initialize, {k_thread_id, group, 1, k_image, k_thread_attr, 0});
    REQUIRE(result(ppu) == k_ok);
    syscall(ppu, k_group_start, {group});
    CHECK(result(ppu) == k_ok);
}

TEST_CASE_METHOD(SpuKernelTest, "a thread gets its image, entry point and arguments") {
    write_program({"stop 0x102"});
    for (u32 i = 0; i < 4; ++i) {
        memory.write<u64>(k_arguments + 8 * i, 10 + i);
    }
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    SpuThread& thread = spu_thread();
    CHECK(thread.name == "spu");
    CHECK(thread.spe->cpu.state.gpr[3] == Vector::from_dwords(10, 0)); // preferred doubleword
    CHECK(thread.spe->cpu.state.gpr[6] == Vector::from_dwords(13, 0));
    CHECK(thread.status == ThreadStatus::ready);

    CHECK(thread.spe->cpu.local_store.read<u32>(0) == spu::assemble("stop 0x102"));
}

TEST_CASE_METHOD(SpuKernelTest, "spu_thread_exit ends the thread with the mailbox's status") {
    write_program({"il $3, 42", "wrch $ch28, $3", "stop 0x102"});
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    CHECK(spu_thread().status == ThreadStatus::finished);

    syscall(ppu, k_get_exit_status, {threads[0], k_status});
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u32>(k_status) == 42);

    // Its only thread exited, so the group ended.
    syscall(ppu, k_group_join, {group, k_cause, k_status});
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u32>(k_cause) == k_spu_join_all_threads_exit);
}

TEST_CASE_METHOD(SpuKernelTest, "the exit status isn't available while the thread runs") {
    write_program({"stop 0x102"});
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    syscall(ppu, k_get_exit_status, {threads[0], k_status});
    CHECK(result(ppu) == k_error_bad_state);
}

TEST_CASE_METHOD(SpuKernelTest, "spu_thread_group_exit ends every thread of the group") {
    write_program({"il $3, 7", "wrch $ch28, $3", "stop 0x101"});
    const u32 group = create_group(2);
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread(0));
    CHECK(spu_thread(1).status == ThreadStatus::finished);

    syscall(ppu, k_group_join, {group, k_cause, k_status});
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u32>(k_cause) == k_spu_join_group_exit);
    CHECK(memory.read<u32>(k_status) == 7);
}

TEST_CASE_METHOD(SpuKernelTest, "joining a running group waits for it to end") {
    write_program({"stop 0x102"});
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    syscall(ppu, k_group_join, {group, k_cause, k_status});
    CHECK(is_waiting(ppu));

    PpuThread& other = new_thread();
    syscall(other, k_group_join, {group, k_cause, k_status});
    CHECK(result(other) == k_error_busy);

    run_spu(spu_thread());
    CHECK_FALSE(is_waiting(ppu));
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u32>(k_cause) == k_spu_join_all_threads_exit);
}

TEST_CASE_METHOD(SpuKernelTest, "terminating a group reports the given value") {
    write_program({"br 0"});
    const u32 group = create_group();
    syscall(ppu, k_group_terminate, {group, 5});
    CHECK(result(ppu) == k_error_bad_state); // not running yet
    syscall(ppu, k_group_start, {group});
    syscall(ppu, k_group_destroy, {group});
    CHECK(result(ppu) == k_error_busy);

    syscall(ppu, k_group_terminate, {group, 5});
    CHECK(result(ppu) == k_ok);
    CHECK(spu_thread().status == ThreadStatus::finished);
    syscall(ppu, k_group_join, {group, k_cause, k_status});
    CHECK(memory.read<u32>(k_cause) == k_spu_join_terminated);
    CHECK(memory.read<u32>(k_status) == 5);

    syscall(ppu, k_group_destroy, {group});
    CHECK(result(ppu) == k_ok);
    CHECK(kernel.spu_threads().empty());
}

TEST_CASE_METHOD(SpuKernelTest, "a restarted group begins from its image again") {
    // The program counts its runs in a word of local store it also changes.
    write_program(
        {"lqa $3, 0x100", "ai $3, $3, 1", "stqa $3, 0x100", "wrch $ch28, $3", "stop 0x102"});
    memory.write<u64>(k_arguments, 0);
    const u32 group = create_group();
    for (int i = 0; i < 2; ++i) {
        syscall(ppu, k_group_start, {group});
        REQUIRE(result(ppu) == k_ok);
        run_spu(spu_thread());
        syscall(ppu, k_get_exit_status, {threads[0], k_status});
        CHECK(memory.read<u32>(k_status) == 1); // not 2: the local store was reloaded
        syscall(ppu, k_group_join, {group, k_cause, k_status});
    }
}

TEST_CASE_METHOD(SpuKernelTest, "new arguments apply at the next start") {
    write_program({"wrch $ch28, $4", "stop 0x102"}); // exit with the second argument's high word
    const u32 group = create_group();
    memory.write<u64>(k_arguments + 8, u64{99} << 32);
    syscall(ppu, k_set_argument, {threads[0], k_arguments});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    syscall(ppu, k_get_exit_status, {threads[0], k_status});
    CHECK(memory.read<u32>(k_status) == 99);
}

TEST_CASE_METHOD(SpuKernelTest, "the PPU writes the inbound mailbox, four entries deep") {
    write_program(
        {"rdch $3, $ch29", "rdch $4, $ch29", "a $3, $3, $4", "wrch $ch28, $3", "stop 0x102"});
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});

    // The SPU stalls on the empty mailbox, and wakes when the PPU fills it.
    CHECK(run_spu(spu_thread()).exit == spu::SpeExit::stalled);
    CHECK(spu_thread().status == ThreadStatus::waiting);
    kernel.update_spu_threads();
    CHECK(spu_thread().status == ThreadStatus::waiting);

    for (u32 value : {30u, 12u, 0u, 0u}) {
        syscall(ppu, k_write_spu_mb, {threads[0], value});
        CHECK(result(ppu) == k_ok);
    }
    syscall(ppu, k_write_spu_mb, {threads[0], 1});
    CHECK(result(ppu) == k_error_busy); // UNVERIFIED: what LV2 does when it's full

    kernel.update_spu_threads();
    CHECK(spu_thread().status == ThreadStatus::ready);
    run_spu(spu_thread());
    syscall(ppu, k_get_exit_status, {threads[0], k_status});
    CHECK(memory.read<u32>(k_status) == 42);
}

TEST_CASE_METHOD(SpuKernelTest, "signals honor the OR mode set by the configuration") {
    write_program({"rdch $3, $ch4", "wrch $ch28, $3", "stop 0x102"});
    const u32 group = create_group();
    syscall(ppu, k_set_spu_cfg, {threads[0], 2}); // signal 2 in OR mode
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_set_spu_cfg, {threads[0], 4});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_group_start, {group});

    syscall(ppu, k_write_snr, {threads[0], 1, 0x10});
    syscall(ppu, k_write_snr, {threads[0], 1, 0x01});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_write_snr, {threads[0], 2, 0x01});
    CHECK(result(ppu) == k_error_invalid);

    run_spu(spu_thread());
    syscall(ppu, k_get_exit_status, {threads[0], k_status});
    CHECK(memory.read<u32>(k_status) == 0x11);
}

TEST_CASE_METHOD(SpuKernelTest, "the PPU reads and writes local store") {
    write_program({"stop 0x102"});
    create_group();
    syscall(ppu, k_write_ls, {threads[0], 0x1000, 0x1122'3344'5566'7788, 8});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_write_ls, {threads[0], 0x1002, 0xabcd, 2});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_read_ls, {threads[0], 0x1000, k_value, 4});
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u64>(k_value) == 0x1122'abcd);

    syscall(ppu, k_write_ls, {threads[0], 0x1001, 0, 2});
    CHECK(result(ppu) == k_error_invalid); // misaligned
    syscall(ppu, k_read_ls, {threads[0], 0x40000, k_value, 4});
    CHECK(result(ppu) == k_error_invalid); // outside local store
    syscall(ppu, k_read_ls, {threads[0], 0, k_value, 3});
    CHECK(result(ppu) == k_error_invalid);
}

TEST_CASE_METHOD(SpuKernelTest, "unknown stop signals are not handled") {
    write_program({"stop 0x2000"});
    create_group();
    syscall(ppu, k_group_start, {memory.read<u32>(k_group_id)});
    const spu::SpeRun run = spu::run(*spu_thread().spe, 100);
    REQUIRE(run.exit == spu::SpeExit::stopped);
    CHECK_FALSE(kernel.handle_spu_stop(spu_thread(), run.signal));
}

TEST_CASE_METHOD(SpuKernelTest, "the scheduler takes turns between PPU and SPU threads") {
    write_program({"br 0"});
    const u32 group = create_group(2);
    syscall(ppu, k_group_start, {group});
    CHECK(kernel.next_thread().ppu == &ppu);
    CHECK(kernel.next_thread().spu == &spu_thread(0));
    CHECK(kernel.next_thread().spu == &spu_thread(1));
    CHECK(kernel.next_thread().ppu == &ppu);
}

TEST_CASE_METHOD(SpuKernelTest, "a DMA wait is a timeout the scheduler can skip to") {
    // mfc_get 16 bytes from the data page, then wait for tag 0.
    write_program({"il $10, 0x100", "ilhu $11, 0x2", "il $12, 16", "il $14, 0x40",
                   "wrch $ch16, $10", "wrch $ch18, $11", "wrch $ch19, $12", "wrch $ch21, $14",
                   "il $16, 1", "il $17, 2", "wrch $ch22, $16", "wrch $ch23, $17", "rdch $3, $ch24",
                   "stop 0x102"});
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    kernel.wait(ppu, Wait::on(WaitKind::sleep), 0);

    CHECK(run_spu(spu_thread()).exit == spu::SpeExit::stalled);
    CHECK_FALSE(kernel.next_thread());
    REQUIRE(kernel.skip_to_next_timeout());
    kernel.update_spu_threads();
    CHECK(kernel.next_thread().spu == &spu_thread());
}

TEST_CASE_METHOD(SpuKernelTest, "a stalled SPU thread shows up in deadlock reports") {
    write_program({"rdch $3, $ch29"});
    create_group();
    syscall(ppu, k_group_start, {memory.read<u32>(k_group_id)});
    run_spu(spu_thread());
    CHECK(kernel.describe_waits().find("waits on channel 29 (SPU_RdInMbox)") != std::string::npos);
}

// ---- Events ----

// spu_thread_send_event(3, 0x1234, 0x99), as PSL1GHT's libsputhread does it, then exits with the
// result.
constexpr std::initializer_list<std::string_view> k_send_event = {
    "il $5, 0x99",     "wrch $ch28, $5",                    // data1
    "ilhu $8, 0x0300", "iohl $8, 0x1234", "wrch $ch30, $8", // port 3, data0
    "rdch $3, $ch29",  "wrch $ch28, $3",  "stop 0x102",
};

TEST_CASE_METHOD(SpuKernelTest, "an SPU thread sends an event through a connected port") {
    write_program(k_send_event);
    const u32 group = create_group();
    const u32 queue = create_queue(k_event_queue_ppu);
    syscall(ppu, k_connect_event, {threads[0], queue, k_spu_thread_event_user, 3});
    REQUIRE(result(ppu) == k_ok);
    syscall(ppu, k_group_start, {group});

    CHECK(run_spu(spu_thread()).exit == spu::SpeExit::interrupt);
    run_spu(spu_thread());
    CHECK(exit_status() == k_ok);

    syscall(ppu, k_queue_receive, {queue, 0, 0});
    REQUIRE(result(ppu) == k_ok);
    const auto& gpr = ppu.cpu.state.gpr;
    CHECK(gpr[4] == k_spu_thread_event_user_key);
    CHECK(gpr[5] == threads[0]);
    CHECK(gpr[6] == 0x3'0000'1234); // the port, then data0
    CHECK(gpr[7] == 0x99);
}

TEST_CASE_METHOD(SpuKernelTest, "sending through an unconnected port fails") {
    write_program(k_send_event);
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    run_spu(spu_thread());
    CHECK(exit_status() == k_error_not_connected);
}

TEST_CASE_METHOD(SpuKernelTest, "a receiving PPU thread is woken by an SPU's event") {
    write_program(k_send_event);
    const u32 group = create_group();
    const u32 queue = create_queue(k_event_queue_ppu);
    syscall(ppu, k_connect_event, {threads[0], queue, k_spu_thread_event_user, 3});
    syscall(ppu, k_group_start, {group});
    syscall(ppu, k_queue_receive, {queue, 0, 0});
    CHECK(is_waiting(ppu));

    run_spu(spu_thread());
    CHECK_FALSE(is_waiting(ppu));
    CHECK(result(ppu) == k_ok);
    CHECK(ppu.cpu.state.gpr[7] == 0x99);
}

TEST_CASE_METHOD(SpuKernelTest, "a thrown event gets no reply") {
    write_program({"il $5, 1", "wrch $ch28, $5", "ilhu $8, 0x4300", "wrch $ch30, $8",
                   "rchcnt $3, $ch29", "wrch $ch28, $3", "stop 0x102"});
    const u32 group = create_group();
    const u32 queue = create_queue(k_event_queue_ppu);
    syscall(ppu, k_connect_event, {threads[0], queue, k_spu_thread_event_user, 3});
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    run_spu(spu_thread());
    CHECK(exit_status() == 0); // the inbound mailbox stayed empty
    CHECK(kernel.find_object<EventQueue>(queue)->events.size() == 1);
}

TEST_CASE_METHOD(SpuKernelTest, "an SPU port connects to one queue at a time") {
    write_program({"stop 0x102"});
    create_group();
    const u32 queue = create_queue(k_event_queue_ppu);
    syscall(ppu, k_connect_event, {threads[0], queue, k_spu_thread_event_user, 64});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_connect_event, {threads[0], queue, k_spu_thread_event_user, 63});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_connect_event, {threads[0], queue, k_spu_thread_event_user, 63});
    CHECK(result(ppu) == k_error_is_connected);
    syscall(ppu, k_disconnect_event, {threads[0], k_spu_thread_event_user, 63});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_disconnect_event, {threads[0], k_spu_thread_event_user, 63});
    CHECK(result(ppu) == k_error_not_connected);
}

// spu_thread_receive_event(5, ...) (stop 0x110) or spu_thread_tryreceive_event (stop 0x111),
// then exits with the error, or with the three data words added together.
std::vector<std::string_view> receive_event(std::string_view stop) {
    return {"il $3, 5",       "wrch $ch28, $3", stop,
            "rdch $3, $ch29", "brnz $3, 0x28", // an error: exit with it
            "rdch $4, $ch29", "rdch $5, $ch29", "rdch $6, $ch29",
            "a $3, $4, $5",   "a $3, $3, $6",   "wrch $ch28, $3",
            "stop 0x102"};
}

TEST_CASE_METHOD(SpuKernelTest, "an SPU thread receives a waiting event from a bound queue") {
    write_program(receive_event("stop 0x110"));
    const u32 group = create_group();
    const u32 queue = create_queue(k_event_queue_spu);
    syscall(ppu, k_bind_queue, {threads[0], queue, 5});
    REQUIRE(result(ppu) == k_ok);
    syscall(ppu, k_port_send, {create_port(queue), 1, 20, 300});
    REQUIRE(result(ppu) == k_ok);

    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread()); // to the stop
    run_spu(spu_thread());
    CHECK(exit_status() == 321);
}

TEST_CASE_METHOD(SpuKernelTest, "an SPU thread waits for an event on an empty queue") {
    write_program(receive_event("stop 0x110"));
    const u32 group = create_group();
    const u32 queue = create_queue(k_event_queue_spu);
    syscall(ppu, k_bind_queue, {threads[0], queue, 5});
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    CHECK(spu_thread().status == ThreadStatus::waiting);
    kernel.update_spu_threads();
    CHECK(spu_thread().status == ThreadStatus::waiting); // not until an event arrives
    CHECK(kernel.describe_waits().find("event queue") != std::string::npos);
    CHECK_FALSE(kernel.skip_to_next_timeout()); // no deadline: only an event ends the wait

    syscall(ppu, k_port_send, {create_port(queue), 4, 5, 6});
    CHECK(spu_thread().status == ThreadStatus::ready);
    CHECK(kernel.find_object<EventQueue>(queue)->events.empty());
    run_spu(spu_thread());
    CHECK(exit_status() == 15);
}

TEST_CASE_METHOD(SpuKernelTest, "tryreceive on an empty queue fails instead of waiting") {
    write_program(receive_event("stop 0x111"));
    const u32 group = create_group();
    const u32 queue = create_queue(k_event_queue_spu);
    syscall(ppu, k_bind_queue, {threads[0], queue, 5});
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    run_spu(spu_thread());
    CHECK(exit_status() == k_error_busy);
}

TEST_CASE_METHOD(SpuKernelTest, "receiving through an unbound number fails") {
    write_program(receive_event("stop 0x110"));
    const u32 group = create_group();
    syscall(ppu, k_group_start, {group});
    run_spu(spu_thread());
    run_spu(spu_thread());
    CHECK(exit_status() == k_error_no_such_id);
}

TEST_CASE_METHOD(SpuKernelTest, "only SPU queues can be bound, each number once") {
    write_program({"stop 0x102"});
    create_group();
    syscall(ppu, k_bind_queue, {threads[0], create_queue(k_event_queue_ppu), 5});
    CHECK(result(ppu) == k_error_invalid);
    const u32 queue = create_queue(k_event_queue_spu);
    syscall(ppu, k_bind_queue, {threads[0], queue, 5});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_bind_queue, {threads[0], queue, 5});
    CHECK(result(ppu) == k_error_busy);
    syscall(ppu, k_unbind_queue, {threads[0], 5});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_unbind_queue, {threads[0], 5});
    CHECK(result(ppu) == k_error_no_such_id);
}
