// Event queues and event ports.
//
// An event queue holds events (four 64-bit words) until a thread receives them. Senders reach a
// queue through a port: a PPU event port, or one of an SPU thread's 64 ports. A PPU queue is
// received from by PPU threads; an SPU queue by the SPU threads it is bound to.

#include "lv2/kernel.h"

#include "common/log.h"

namespace lv2 {

namespace {

// Puts a received event where sys_event_queue_receive returns it: in r4-r7. PSL1GHT's
// sysEventQueueReceive reads it from there (REG_PASS_SYS_EVENT_QUEUE_RECEIVE).
void give_event(PpuThread& thread, const Event& event) {
    auto& gpr = thread.cpu.state.gpr;
    gpr[4] = event.source;
    gpr[5] = event.data1;
    gpr[6] = event.data2;
    gpr[7] = event.data3;
}

// Answers an SPU thread through its inbound mailbox: the result, then for a received event its
// three data words. UNVERIFIED: an SPU thread gets only the low 32 bits of each.
void reply_to_spu(SpuThread& thread, s32 result, const Event* event = nullptr) {
    spu::Channels& channels = thread.spe->channels;
    channels.write_in_mbox(static_cast<u32>(result));
    if (!event) return;
    channels.write_in_mbox(static_cast<u32>(event->data1));
    channels.write_in_mbox(static_cast<u32>(event->data2));
    channels.write_in_mbox(static_cast<u32>(event->data3));
}

} // namespace

// ---- Queues ----

s32 Kernel::create_event_queue(u32 protocol, u32 type, u64 key, s32 size, u32& queue_id) {
    if (protocol < k_protocol_fifo || protocol > k_protocol_priority_inherit) {
        return k_error_invalid;
    }
    if (type != k_event_queue_ppu && type != k_event_queue_spu) return k_error_invalid;
    if (size <= 0 || size > static_cast<s32>(k_max_event_queue_size)) return k_error_invalid;
    // Other processes find a queue by its key, so a key can only be used once.
    if (key != 0) {
        for (const auto& [id, object] : objects_) {
            const auto* queue = dynamic_cast<const EventQueue*>(object.get());
            if (queue && queue->key == key) return k_error_exists;
        }
    }

    auto queue = std::make_unique<EventQueue>();
    queue->type = type;
    queue->wake_by_priority = protocol != k_protocol_fifo;
    queue->key = key;
    queue->size = static_cast<u32>(size);
    queue_id = add_object(std::move(queue));
    return k_ok;
}

s32 Kernel::destroy_event_queue(u32 queue_id, u32 mode) {
    if (!find_object<EventQueue>(queue_id)) return k_error_no_such_id;
    if (mode != 0 && mode != k_event_queue_force_destroy) return k_error_invalid;

    const bool busy = has_waiters(WaitKind::event_queue, queue_id) || first_spu_waiter(queue_id);
    if (busy && mode != k_event_queue_force_destroy) return k_error_busy;

    // Forced: the waiters' receives fail. Ports still connected to the queue now send nowhere.
    while (PpuThread* waiter = first_waiter(WaitKind::event_queue, queue_id, false)) {
        wake(*waiter, k_error_canceled);
    }
    while (SpuThread* waiter = first_spu_waiter(queue_id)) {
        // UNVERIFIED: an SPU thread's receive fails the same way.
        reply_to_spu(*waiter, k_error_canceled);
        waiter->waiting_queue = 0;
        waiter->status = ThreadStatus::ready;
    }
    remove_object(queue_id);
    return k_ok;
}

std::optional<s32> Kernel::receive_event(PpuThread& thread, u32 queue_id, u64 timeout_us) {
    EventQueue* queue = find_object<EventQueue>(queue_id);
    if (!queue) return k_error_no_such_id;
    // UNVERIFIED: the error for receiving from an SPU queue on the PPU.
    if (queue->type != k_event_queue_ppu) return k_error_invalid;

    if (!queue->events.empty()) {
        give_event(thread, queue->events.front());
        queue->events.pop_front();
        return k_ok;
    }
    wait(thread, Wait::on(WaitKind::event_queue, queue_id), timeout_us);
    return std::nullopt;
}

s32 Kernel::try_receive_events(u32 queue_id, u32 max, std::vector<Event>& events) {
    EventQueue* queue = find_object<EventQueue>(queue_id);
    if (!queue) return k_error_no_such_id;
    if (queue->type != k_event_queue_ppu) return k_error_invalid;

    while (events.size() < max && !queue->events.empty()) {
        events.push_back(queue->events.front());
        queue->events.pop_front();
    }
    return k_ok;
}

s32 Kernel::drain_event_queue(u32 queue_id) {
    EventQueue* queue = find_object<EventQueue>(queue_id);
    if (!queue) return k_error_no_such_id;
    queue->events.clear();
    return k_ok;
}

s32 Kernel::send_event(u32 queue_id, const Event& event) {
    EventQueue* queue = find_object<EventQueue>(queue_id);
    if (!queue) return k_error_not_connected;

    if (queue->type == k_event_queue_ppu) {
        if (PpuThread* waiter =
                first_waiter(WaitKind::event_queue, queue_id, queue->wake_by_priority)) {
            give_event(*waiter, event);
            wake(*waiter, k_ok);
            return k_ok;
        }
    } else if (SpuThread* waiter = first_spu_waiter(queue_id)) {
        reply_to_spu(*waiter, k_ok, &event);
        waiter->waiting_queue = 0;
        waiter->status = ThreadStatus::ready;
        return k_ok;
    }

    if (queue->events.size() >= queue->size) return k_error_busy;
    queue->events.push_back(event);
    return k_ok;
}

// The SPU thread that has waited longest on the queue.
SpuThread* Kernel::first_spu_waiter(u32 queue_id) {
    SpuThread* first = nullptr;
    for (const auto& thread : spu_threads_) {
        if (thread->status != ThreadStatus::waiting || thread->waiting_queue != queue_id) continue;
        if (!first || thread->wait_sequence < first->wait_sequence) first = thread.get();
    }
    return first;
}

// ---- PPU event ports ----

s32 Kernel::create_event_port(u32 type, u64 name, u32& port_id) {
    if (type != k_event_port_local) return k_error_invalid;
    auto port = std::make_unique<EventPort>();
    port->name = name;
    port_id = add_object(std::move(port));
    // UNVERIFIED: what LV2 names a port created with SYS_EVENT_PORT_NO_NAME (0). We use its ID,
    // so receivers can still tell ports apart.
    if (name == 0) find_object<EventPort>(port_id)->name = port_id;
    return k_ok;
}

s32 Kernel::destroy_event_port(u32 port_id) {
    EventPort* port = find_object<EventPort>(port_id);
    if (!port) return k_error_no_such_id;
    // UNVERIFIED: a connected port can't be destroyed.
    if (port->queue != 0) return k_error_is_connected;
    remove_object(port_id);
    return k_ok;
}

s32 Kernel::connect_event_port(u32 port_id, u32 queue_id) {
    EventPort* port = find_object<EventPort>(port_id);
    if (!port || !find_object<EventQueue>(queue_id)) return k_error_no_such_id;
    if (port->queue != 0) return k_error_is_connected;
    port->queue = queue_id;
    return k_ok;
}

s32 Kernel::disconnect_event_port(u32 port_id) {
    EventPort* port = find_object<EventPort>(port_id);
    if (!port) return k_error_no_such_id;
    if (port->queue == 0) return k_error_not_connected;
    port->queue = 0;
    return k_ok;
}

s32 Kernel::send_event_from_port(u32 port_id, u64 data1, u64 data2, u64 data3) {
    EventPort* port = find_object<EventPort>(port_id);
    if (!port) return k_error_no_such_id;
    if (port->queue == 0) return k_error_not_connected;
    return send_event(port->queue, {port->name, data1, data2, data3});
}

// ---- SPU threads ----

s32 Kernel::connect_spu_thread_event(u32 thread_id, u32 queue_id, u32 type, u32 port) {
    SpuThread* thread = find_spu_thread(thread_id);
    if (!thread || !find_object<EventQueue>(queue_id)) return k_error_no_such_id;
    if (type == k_spu_thread_event_dma) {
        log_warning("lv2", "SPU thread DMA events are not supported yet");
        return k_error_not_implemented;
    }
    if (type != k_spu_thread_event_user || port >= k_spu_event_port_count) return k_error_invalid;
    if (thread->event_ports[port] != 0) return k_error_is_connected;
    thread->event_ports[port] = queue_id;
    return k_ok;
}

s32 Kernel::disconnect_spu_thread_event(u32 thread_id, u32 type, u32 port) {
    SpuThread* thread = find_spu_thread(thread_id);
    if (!thread) return k_error_no_such_id;
    if (type != k_spu_thread_event_user || port >= k_spu_event_port_count) return k_error_invalid;
    if (thread->event_ports[port] == 0) return k_error_not_connected;
    thread->event_ports[port] = 0;
    return k_ok;
}

s32 Kernel::bind_spu_thread_queue(u32 thread_id, u32 queue_id, u32 number) {
    SpuThread* thread = find_spu_thread(thread_id);
    const EventQueue* queue = find_object<EventQueue>(queue_id);
    if (!thread || !queue) return k_error_no_such_id;
    // UNVERIFIED: only SPU queues can be bound, and a number can only be bound once.
    if (queue->type != k_event_queue_spu) return k_error_invalid;
    if (thread->bound_queues.contains(number)) return k_error_busy;
    thread->bound_queues[number] = queue_id;
    return k_ok;
}

s32 Kernel::unbind_spu_thread_queue(u32 thread_id, u32 number) {
    SpuThread* thread = find_spu_thread(thread_id);
    if (!thread) return k_error_no_such_id;
    if (thread->bound_queues.erase(number) == 0) return k_error_no_such_id;
    return k_ok;
}

// The value written to SPU_WrOutIntrMbox is {flag, port, data0}: bit 30 set means "throw" (no
// reply), bits 24-29 are the SPU port and bits 0-23 the event's first data word. The second is
// in the outbound mailbox. A sender that isn't throwing reads the result from its inbound
// mailbox. PSL1GHT's libsputhread: spu_thread_send_event and spu_thread_throw_event.
void Kernel::handle_spu_interrupt(SpuThread& thread) {
    spu::Channels& channels = thread.spe->channels;
    const u32 value = channels.read_out_intr_mbox().value_or(0);
    // UNVERIFIED: an empty outbound mailbox gives 0.
    const u32 data1 = channels.read_out_mbox().value_or(0);

    const bool is_throw = (value & 0x4000'0000) != 0;
    const u32 port = (value >> 24) & 0x3f;
    const u32 data0 = value & 0x00ff'ffff;

    const Event event{k_spu_thread_event_user_key, thread.id, (u64{port} << 32) | data0, data1};
    const u32 queue = thread.event_ports[port];
    const s32 result = queue == 0 ? k_error_not_connected : send_event(queue, event);
    if (!is_throw) reply_to_spu(thread, result);
}

// spu_thread_receive_event (can_wait) or spu_thread_tryreceive_event.
void Kernel::receive_spu_event(SpuThread& thread, bool can_wait) {
    const u32 number = thread.spe->channels.read_out_mbox().value_or(0);
    const auto binding = thread.bound_queues.find(number);
    EventQueue* queue =
        binding == thread.bound_queues.end() ? nullptr : find_object<EventQueue>(binding->second);
    // UNVERIFIED: the error for a number with no queue bound.
    if (!queue) return reply_to_spu(thread, k_error_no_such_id);

    if (!queue->events.empty()) {
        const Event event = queue->events.front();
        queue->events.pop_front();
        return reply_to_spu(thread, k_ok, &event);
    }
    // UNVERIFIED: the error tryreceive gives when the queue is empty.
    if (!can_wait) return reply_to_spu(thread, k_error_busy);

    thread.status = ThreadStatus::waiting;
    thread.waiting_queue = binding->second;
    thread.wait_sequence = next_wait_sequence_++;
}

} // namespace lv2
