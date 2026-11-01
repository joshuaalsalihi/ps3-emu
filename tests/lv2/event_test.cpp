// Event queues and event ports, driven through their syscalls.

#include "support/kernel_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace lv2;
using test::KernelTest;

namespace {

// Syscall numbers. psdevwiki, "LV2 Functions and Syscalls".
constexpr u32 k_queue_create = 128;
constexpr u32 k_queue_destroy = 129;
constexpr u32 k_queue_receive = 130;
constexpr u32 k_queue_tryreceive = 131;
constexpr u32 k_queue_drain = 133;
constexpr u32 k_port_create = 134;
constexpr u32 k_port_destroy = 135;
constexpr u32 k_port_connect = 136;
constexpr u32 k_port_disconnect = 137;
constexpr u32 k_port_send = 138;

constexpr u64 k_attr = KernelTest::k_data;
constexpr u64 k_queue_id = KernelTest::k_data + 0x20;
constexpr u64 k_port_id = KernelTest::k_data + 0x24;
constexpr u64 k_number = KernelTest::k_data + 0x28;
constexpr u64 k_events = KernelTest::k_data + 0x100;

class EventTest : public KernelTest {
public:
    u32 create_queue(u32 size = 4, u32 protocol = k_protocol_fifo, u64 key = 0) {
        memory.write<u32>(k_attr, protocol);
        memory.write<u32>(k_attr + 4, k_event_queue_ppu);
        syscall(ppu, k_queue_create, {k_queue_id, k_attr, key, size});
        REQUIRE(result(ppu) == k_ok);
        return memory.read<u32>(k_queue_id);
    }

    // A port with `name`, connected to `queue`.
    u32 create_port(u32 queue, u64 name) {
        syscall(ppu, k_port_create, {k_port_id, k_event_port_local, name});
        REQUIRE(result(ppu) == k_ok);
        const u32 port = memory.read<u32>(k_port_id);
        syscall(ppu, k_port_connect, {port, queue});
        REQUIRE(result(ppu) == k_ok);
        return port;
    }

    // The event a receive left in r4-r7.
    static Event received(const PpuThread& thread) {
        const auto& gpr = thread.cpu.state.gpr;
        return {gpr[4], gpr[5], gpr[6], gpr[7]};
    }

    PpuThread& ppu = new_thread();
};

} // namespace

TEST_CASE_METHOD(EventTest, "queue creation checks its attributes") {
    memory.write<u32>(k_attr, k_protocol_fifo);
    memory.write<u32>(k_attr + 4, 3); // no such type
    syscall(ppu, k_queue_create, {k_queue_id, k_attr, 0, 4});
    CHECK(result(ppu) == k_error_invalid);

    memory.write<u32>(k_attr + 4, k_event_queue_ppu);
    syscall(ppu, k_queue_create, {k_queue_id, k_attr, 0, 0});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_queue_create, {k_queue_id, k_attr, 0, 128});
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_queue_create, {k_queue_id, k_attr, 0, 127});
    CHECK(result(ppu) == k_ok);
}

TEST_CASE_METHOD(EventTest, "a queue key can only be used once") {
    create_queue(4, k_protocol_fifo, 0x1234);
    syscall(ppu, k_queue_create, {k_queue_id, k_attr, 0x1234, 4});
    CHECK(result(ppu) == k_error_exists);
    create_queue(4, k_protocol_fifo, 0); // local queues have no key to clash
    create_queue(4, k_protocol_fifo, 0);
}

TEST_CASE_METHOD(EventTest, "a sent event is received with the port's name as its source") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 0xfee1dead);
    syscall(ppu, k_port_send, {port, 1, 2, 3});
    CHECK(result(ppu) == k_ok);

    syscall(ppu, k_queue_receive, {queue, k_events, 0});
    CHECK(result(ppu) == k_ok);
    const Event event = received(ppu);
    CHECK(event.source == 0xfee1dead);
    CHECK(event.data1 == 1);
    CHECK(event.data2 == 2);
    CHECK(event.data3 == 3);
}

TEST_CASE_METHOD(EventTest, "events are received in the order they were sent") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 1);
    syscall(ppu, k_port_send, {port, 10, 0, 0});
    syscall(ppu, k_port_send, {port, 20, 0, 0});
    syscall(ppu, k_queue_receive, {queue, k_events, 0});
    CHECK(received(ppu).data1 == 10);
    syscall(ppu, k_queue_receive, {queue, k_events, 0});
    CHECK(received(ppu).data1 == 20);
}

TEST_CASE_METHOD(EventTest, "receiving from an empty queue waits for an event") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 1);
    PpuThread& receiver = new_thread();
    syscall(receiver, k_queue_receive, {queue, k_events, 0});
    CHECK(is_waiting(receiver));

    syscall(ppu, k_port_send, {port, 5, 6, 7});
    CHECK_FALSE(is_waiting(receiver));
    CHECK(result(receiver) == k_ok);
    CHECK(received(receiver).data3 == 7);
    // It went straight to the waiter.
    CHECK(kernel.find_object<EventQueue>(queue)->events.empty());
}

TEST_CASE_METHOD(EventTest, "a receive can time out") {
    const u32 queue = create_queue();
    syscall(ppu, k_queue_receive, {queue, k_events, 100});
    CHECK(is_waiting(ppu));
    clock.advance(100 * VirtualClock::k_cpu_frequency / 1'000'000);
    kernel.wake_timed_out_threads();
    CHECK(result(ppu) == k_error_timed_out);
}

TEST_CASE_METHOD(EventTest, "a priority queue wakes its most urgent waiter first") {
    const u32 queue = create_queue(4, k_protocol_priority);
    const u32 port = create_port(queue, 1);
    PpuThread& low = new_thread(2000);
    PpuThread& high = new_thread(500);
    syscall(low, k_queue_receive, {queue, k_events, 0});
    syscall(high, k_queue_receive, {queue, k_events, 0});

    syscall(ppu, k_port_send, {port, 0, 0, 0});
    CHECK_FALSE(is_waiting(high));
    CHECK(is_waiting(low));
}

TEST_CASE_METHOD(EventTest, "sending to a full queue fails") {
    const u32 queue = create_queue(2);
    const u32 port = create_port(queue, 1);
    syscall(ppu, k_port_send, {port, 0, 0, 0});
    syscall(ppu, k_port_send, {port, 0, 0, 0});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_port_send, {port, 0, 0, 0});
    CHECK(result(ppu) == k_error_busy);
}

TEST_CASE_METHOD(EventTest, "tryreceive copies out the waiting events without waiting") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 0x77);
    syscall(ppu, k_queue_tryreceive, {queue, k_events, 4, k_number});
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u32>(k_number) == 0);

    for (u64 i = 1; i <= 3; ++i) {
        syscall(ppu, k_port_send, {port, i, i * 10, i * 100});
    }
    syscall(ppu, k_queue_tryreceive, {queue, k_events, 2, k_number});
    CHECK(result(ppu) == k_ok);
    CHECK(memory.read<u32>(k_number) == 2);
    CHECK(memory.read<u64>(k_events) == 0x77); // sys_event_t: source, data1, data2, data3
    CHECK(memory.read<u64>(k_events + 8) == 1);
    CHECK(memory.read<u64>(k_events + 32 + 24) == 200);
    CHECK(kernel.find_object<EventQueue>(queue)->events.size() == 1);
}

TEST_CASE_METHOD(EventTest, "draining empties the queue") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 1);
    syscall(ppu, k_port_send, {port, 0, 0, 0});
    syscall(ppu, k_queue_drain, {queue});
    CHECK(result(ppu) == k_ok);
    CHECK(kernel.find_object<EventQueue>(queue)->events.empty());
}

TEST_CASE_METHOD(EventTest, "ports connect to one queue at a time") {
    const u32 queue = create_queue();
    syscall(ppu, k_port_create, {k_port_id, 2, 0}); // not a local port
    CHECK(result(ppu) == k_error_invalid);
    syscall(ppu, k_port_create, {k_port_id, k_event_port_local, 0});
    const u32 port = memory.read<u32>(k_port_id);

    syscall(ppu, k_port_send, {port, 0, 0, 0});
    CHECK(result(ppu) == k_error_not_connected);
    syscall(ppu, k_port_disconnect, {port});
    CHECK(result(ppu) == k_error_not_connected);

    syscall(ppu, k_port_connect, {port, queue});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_port_connect, {port, queue});
    CHECK(result(ppu) == k_error_is_connected);
    syscall(ppu, k_port_destroy, {port});
    CHECK(result(ppu) == k_error_is_connected);

    syscall(ppu, k_port_disconnect, {port});
    CHECK(result(ppu) == k_ok);
    syscall(ppu, k_port_destroy, {port});
    CHECK(result(ppu) == k_ok);
}

TEST_CASE_METHOD(EventTest, "an unnamed port's events still say which port sent them") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 0);
    syscall(ppu, k_port_send, {port, 0, 0, 0});
    syscall(ppu, k_queue_receive, {queue, k_events, 0});
    CHECK(received(ppu).source == port);
}

TEST_CASE_METHOD(EventTest, "a queue with waiters is destroyed only by force") {
    const u32 queue = create_queue();
    const u32 port = create_port(queue, 1);
    PpuThread& receiver = new_thread();
    syscall(receiver, k_queue_receive, {queue, k_events, 0});

    syscall(ppu, k_queue_destroy, {queue, 0});
    CHECK(result(ppu) == k_error_busy);
    syscall(ppu, k_queue_destroy, {queue, k_event_queue_force_destroy});
    CHECK(result(ppu) == k_ok);
    CHECK(result(receiver) == k_error_canceled);

    // The port still exists, but its queue doesn't.
    syscall(ppu, k_port_send, {port, 0, 0, 0});
    CHECK(result(ppu) == k_error_not_connected);
}
