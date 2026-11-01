// Event queue and event port syscalls. Numbers and signatures: psdevwiki, "LV2 Functions and
// Syscalls"; structure layouts: PSL1GHT, sys/event_queue.h. The logic is in kernel_event.cpp.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

constexpr u32 k_event_size = 32; // sys_event_t: four u64s

u32 id_arg(const PpuThread& thread, u32 index) {
    return static_cast<u32>(thread.arg(index));
}

bool can_write(Kernel& kernel, u32 address, u64 size) {
    return kernel.memory.is_accessible(address, size, Memory::k_write);
}

// int sys_event_queue_create(sys_event_queue_t* equeue_id, sys_event_queue_attribute_t* attr,
//                            sys_ipc_key_t event_queue_key, int size)
// attr: {u32 protocol, s32 type, char name[8]}.
void sys_event_queue_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const u32 attr = thread.pointer_arg(1);
    const u32 protocol = kernel.memory.read<u32>(attr);
    const u32 type = kernel.memory.read<u32>(attr + 4);
    if (!can_write(kernel, id_out, 4)) return thread.set_result(k_error_bad_address);

    u32 queue_id = 0;
    const s32 result = kernel.create_event_queue(protocol, type, thread.arg(2),
                                                 static_cast<s32>(thread.arg(3)), queue_id);
    if (result == k_ok) kernel.memory.write<u32>(id_out, queue_id);
    thread.set_result(result);
}

// int sys_event_queue_destroy(sys_event_queue_t equeue_id, int mode)
void sys_event_queue_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_event_queue(id_arg(thread, 0), id_arg(thread, 1)));
}

// int sys_event_queue_receive(sys_event_queue_t equeue_id, sys_event_t* event, usecond_t timeout)
// The event comes back in r4-r7, not through the pointer (see Kernel::receive_event).
void sys_event_queue_receive(Kernel& kernel, PpuThread& thread) {
    const auto result = kernel.receive_event(thread, id_arg(thread, 0), thread.arg(2));
    if (result) thread.set_result(*result);
}

// int sys_event_queue_tryreceive(sys_event_queue_t equeue_id, sys_event_t* event_array, int size,
//                                int* number)
void sys_event_queue_tryreceive(Kernel& kernel, PpuThread& thread) {
    const u32 array = thread.pointer_arg(1);
    const s32 size = static_cast<s32>(thread.arg(2));
    const u32 number_out = thread.pointer_arg(3);
    if (size < 0) return thread.set_result(k_error_invalid);
    if (!can_write(kernel, array, u64{static_cast<u32>(size)} * k_event_size) ||
        !can_write(kernel, number_out, 4)) {
        return thread.set_result(k_error_bad_address);
    }

    std::vector<Event> events;
    const s32 result = kernel.try_receive_events(id_arg(thread, 0), static_cast<u32>(size), events);
    if (result != k_ok) return thread.set_result(result);
    for (u32 i = 0; i < events.size(); ++i) {
        const u32 address = array + i * k_event_size;
        kernel.memory.write<u64>(address, events[i].source);
        kernel.memory.write<u64>(address + 8, events[i].data1);
        kernel.memory.write<u64>(address + 16, events[i].data2);
        kernel.memory.write<u64>(address + 24, events[i].data3);
    }
    kernel.memory.write<u32>(number_out, static_cast<u32>(events.size()));
    thread.set_result(k_ok);
}

// int sys_event_queue_drain(sys_event_queue_t equeue_id)
void sys_event_queue_drain(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.drain_event_queue(id_arg(thread, 0)));
}

// int sys_event_port_create(sys_event_port_t* eport_id, int port_type, uint64_t name)
void sys_event_port_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    if (!can_write(kernel, id_out, 4)) return thread.set_result(k_error_bad_address);

    u32 port_id = 0;
    const s32 result = kernel.create_event_port(id_arg(thread, 1), thread.arg(2), port_id);
    if (result == k_ok) kernel.memory.write<u32>(id_out, port_id);
    thread.set_result(result);
}

// int sys_event_port_destroy(sys_event_port_t eport_id)
void sys_event_port_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_event_port(id_arg(thread, 0)));
}

// int sys_event_port_connect_local(sys_event_port_t event_port_id,
//                                  sys_event_queue_t event_queue_id)
void sys_event_port_connect_local(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.connect_event_port(id_arg(thread, 0), id_arg(thread, 1)));
}

// int sys_event_port_disconnect(sys_event_port_t event_port_id)
void sys_event_port_disconnect(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.disconnect_event_port(id_arg(thread, 0)));
}

// int sys_event_port_send(sys_event_port_t eport_id, uint64_t data1, uint64_t data2,
//                         uint64_t data3)
void sys_event_port_send(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.send_event_from_port(id_arg(thread, 0), thread.arg(1), thread.arg(2),
                                                  thread.arg(3)));
}

constexpr SyscallInfo k_syscalls[] = {
    {128, "sys_event_queue_create", sys_event_queue_create},
    {129, "sys_event_queue_destroy", sys_event_queue_destroy},
    {130, "sys_event_queue_receive", sys_event_queue_receive},
    {131, "sys_event_queue_tryreceive", sys_event_queue_tryreceive},
    {133, "sys_event_queue_drain", sys_event_queue_drain},
    {134, "sys_event_port_create", sys_event_port_create},
    {135, "sys_event_port_destroy", sys_event_port_destroy},
    {136, "sys_event_port_connect_local", sys_event_port_connect_local},
    {137, "sys_event_port_disconnect", sys_event_port_disconnect},
    {138, "sys_event_port_send", sys_event_port_send},
};

} // namespace

std::span<const SyscallInfo> event_syscalls() {
    return k_syscalls;
}

} // namespace lv2
