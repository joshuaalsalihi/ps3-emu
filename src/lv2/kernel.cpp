#include "lv2/kernel.h"

#include "common/log.h"

#include <format>
#include <vector>

namespace lv2 {

namespace {

constexpr u64 k_cycles_per_microsecond = VirtualClock::k_cpu_frequency / 1'000'000;

// Room above a new thread's stack pointer for the ABI's back chain and saved link register.
constexpr u64 k_initial_frame_size = 128;

// UNVERIFIED: the smallest stack LV2 gives a thread.
constexpr u64 k_min_stack_size = 0x4000;

// Thread-local storage layout. The 64-bit PowerPC ELF ABI puts the thread pointer (r13) 0x7000
// bytes past the start of the thread's TLS data. UNVERIFIED: psdevwiki describes a 0x30-byte
// header before the data, which we leave zeroed.
constexpr u64 k_tls_header_size = 0x30;
constexpr u64 k_tls_pointer_offset = 0x7000;

u64 round_up_to_page(u64 size) {
    return (size + Memory::k_page_size - 1) & ~(Memory::k_page_size - 1);
}

const char* wait_kind_name(WaitKind kind) {
    switch (kind) {
    case WaitKind::sleep: return "sleep";
    case WaitKind::join: return "join thread";
    case WaitKind::mutex: return "mutex";
    case WaitKind::cond: return "condition variable";
    case WaitKind::semaphore: return "semaphore";
    case WaitKind::spu_group: return "SPU thread group";
    case WaitKind::event_queue: return "event queue";
    }
    return "?";
}

} // namespace

Kernel::Kernel(Memory& memory, VirtualClock& clock, std::ostream& tty)
    : memory(memory), clock(clock), tty(tty) {
    for (const auto table :
         {process_syscalls(), thread_syscalls(), sync_syscalls(), time_syscalls(),
          memory_syscalls(), tty_syscalls(), fs_syscalls(), spu_syscalls(), event_syscalls()}) {
        for (const SyscallInfo& info : table) {
            syscalls_[info.number] = &info;
        }
    }

    // Return stubs, so a thread's entry function can simply return.
    memory.map(k_stub_page, Memory::k_page_size, Memory::k_read_write);
    memory.write<u32>(k_thread_exit_stub, 0x39600029);      // li r11, 41 (sys_ppu_thread_exit)
    memory.write<u32>(k_thread_exit_stub + 4, 0x44000002);  // sc
    memory.write<u32>(k_process_exit_stub, 0x39600003);     // li r11, 3 (sys_process_exit)
    memory.write<u32>(k_process_exit_stub + 4, 0x44000002); // sc
    memory.protect(k_stub_page, Memory::k_page_size, Memory::k_read);
}

// ---- Syscalls ----

const SyscallInfo* Kernel::find_syscall(u32 number) const {
    const auto it = syscalls_.find(number);
    return it == syscalls_.end() ? nullptr : it->second;
}

void Kernel::handle_syscall(PpuThread& thread) {
    const u64 number = thread.cpu.state.gpr[11];
    const SyscallInfo* info = number <= 0xffff ? find_syscall(static_cast<u32>(number)) : nullptr;
    if (!info) {
        log_warning("lv2", std::format("unimplemented syscall {} at {:#x}", number,
                                       thread.cpu.state.pc - 4));
        thread.set_result(k_error_not_implemented);
        return;
    }

    try {
        info->handler(*this, thread);
    } catch (const MemoryFault&) {
        // A pointer argument was bad. Handlers check before changing kernel state.
        thread.set_result(k_error_bad_address);
    }
}

// ---- Process ----

void Kernel::exit_process(s32 status) {
    exit_status_ = status;
}

// ---- Console ----

void Kernel::write_tty(std::span<const u8> bytes) {
    const u64 end = tty_written_ + bytes.size();
    if (end > tty_shown_) {
        const u64 skip = tty_shown_ > tty_written_ ? tty_shown_ - tty_written_ : 0;
        tty.write(reinterpret_cast<const char*>(bytes.data() + skip),
                  static_cast<std::streamsize>(bytes.size() - skip));
        tty_shown_ = end;
    }
    tty_written_ = end;
}

// ---- Threads ----

PpuThread* Kernel::create_thread(std::string name, u64 entry, u64 toc, u64 argument, s32 priority,
                                 u64 stack_size) {
    if (stack_size > k_stack_area_size) return nullptr; // also keeps the rounding from overflowing
    stack_size = round_up_to_page(std::max(stack_size, k_min_stack_size));
    const u64 tls_size =
        tls_memory_size_ == 0 ? 0 : round_up_to_page(k_tls_header_size + tls_memory_size_);

    // Layout: the thread's TLS, an unmapped guard page, then the stack. The guard page makes a
    // stack overflow fault instead of silently corrupting memory.
    const u64 total = tls_size + Memory::k_page_size + stack_size;
    const auto range = stacks_.allocate(total, Memory::k_page_size);
    if (!range) return nullptr;
    const u64 tls_address = *range;
    const u64 stack_address = tls_address + tls_size + Memory::k_page_size;
    memory.map(stack_address, stack_size, Memory::k_read_write);

    const u32 id = next_id_++;
    auto thread = std::make_unique<PpuThread>(id, std::move(name), priority, memory, clock);
    thread->stack_address = stack_address;
    thread->stack_size = stack_size;

    ppu::State& state = thread->cpu.state;
    state.pc = entry;
    state.gpr[1] = stack_address + stack_size - k_initial_frame_size;
    state.gpr[2] = toc;
    state.gpr[3] = argument;
    state.lr = k_thread_exit_stub;

    if (tls_size != 0) {
        // Copy the initialized part of the template; the rest of fresh memory is already zero.
        memory.map(tls_address, tls_size, Memory::k_read_write);
        std::vector<u8> image(tls_file_size_);
        memory.read_bytes(tls_address_, image);
        memory.write_bytes(tls_address + k_tls_header_size, image);
        state.gpr[13] = tls_address + k_tls_header_size + k_tls_pointer_offset;
    }

    threads_.push_back(std::move(thread));
    return threads_.back().get();
}

s32 Kernel::create_thread_from_descriptor(u32 descriptor, u64 argument, s32 priority,
                                          u64 stack_size, u64 flags, std::string name,
                                          u32& thread_id) {
    // UNVERIFIED: the valid priority range, from the limits psdevwiki notes for set_priority.
    if (priority < 0 || priority > 3071) return k_error_invalid;

    const u32 entry = memory.read<u32>(descriptor);
    const u32 toc = memory.read<u32>(descriptor + 4);
    PpuThread* thread = create_thread(std::move(name), entry, toc, argument, priority, stack_size);
    if (!thread) return k_error_no_memory;

    thread->joinable = (flags & 1) != 0;
    thread_id = thread->id;
    return k_ok;
}

void Kernel::set_tls_template(u64 address, u64 file_size, u64 memory_size) {
    tls_address_ = address;
    tls_file_size_ = file_size;
    tls_memory_size_ = memory_size;
}

void Kernel::start_thread(PpuThread& thread) {
    thread.status = ThreadStatus::ready;
}

void Kernel::exit_thread(PpuThread& thread, u64 exit_value) {
    thread.status = ThreadStatus::finished;
    thread.exit_value = exit_value;

    while (PpuThread* joiner = first_waiter(WaitKind::join, thread.id, false)) {
        if (joiner->wait.output_address != 0) {
            memory.write<u64>(joiner->wait.output_address, exit_value);
        }
        wake(*joiner, k_ok);
        thread.joinable = false; // its exit value has been collected
    }
}

PpuThread* Kernel::find_thread(u32 id) {
    for (const auto& thread : threads_) {
        if (thread->id == id) return thread.get();
    }
    return nullptr;
}

namespace {

bool is_alive(ThreadStatus status) {
    return status == ThreadStatus::ready || status == ThreadStatus::waiting;
}

} // namespace

bool Kernel::all_threads_finished() const {
    for (const auto& thread : threads_) {
        if (is_alive(thread->status)) return false;
    }
    for (const auto& thread : spu_threads_) {
        if (is_alive(thread->status)) return false;
    }
    return true;
}

Runnable Kernel::next_thread() {
    // Look at each thread once, starting after the one picked last time. Positions below
    // threads_.size() are PPU threads; the rest are SPU threads.
    const std::size_t total = threads_.size() + spu_threads_.size();
    for (std::size_t step = 0; step < total; ++step) {
        const std::size_t index = (next_to_schedule_ + step) % total;
        if (index < threads_.size()) {
            if (threads_[index]->status != ThreadStatus::ready) continue;
            next_to_schedule_ = index + 1;
            return {threads_[index].get(), nullptr};
        }
        SpuThread& spu = *spu_threads_[index - threads_.size()];
        if (spu.status != ThreadStatus::ready) continue;
        next_to_schedule_ = index + 1;
        return {nullptr, &spu};
    }
    return {};
}

// ---- Waiting ----

void Kernel::wait(PpuThread& thread, Wait wait, u64 timeout_us) {
    wait.sequence = next_wait_sequence_++;
    if (timeout_us != 0) {
        wait.deadline = clock.cycles() + timeout_us * k_cycles_per_microsecond;
    }
    thread.wait = wait;
    thread.status = ThreadStatus::waiting;
}

void Kernel::wake(PpuThread& thread, s32 result) {
    thread.status = ThreadStatus::ready;
    thread.set_result(result);
}

PpuThread* Kernel::first_waiter(WaitKind kind, u32 object, bool by_priority) {
    PpuThread* best = nullptr;
    for (const auto& thread : threads_) {
        if (thread->status != ThreadStatus::waiting || thread->wait.kind != kind ||
            thread->wait.object != object) {
            continue;
        }
        const bool earlier = !best || thread->wait.sequence < best->wait.sequence;
        const bool more_urgent = best && thread->priority < best->priority;
        const bool same_priority = best && thread->priority == best->priority;
        if (by_priority ? (!best || more_urgent || (same_priority && earlier)) : earlier) {
            best = thread.get();
        }
    }
    return best;
}

bool Kernel::has_waiters(WaitKind kind, u32 object) const {
    for (const auto& thread : threads_) {
        if (thread->status == ThreadStatus::waiting && thread->wait.kind == kind &&
            thread->wait.object == object) {
            return true;
        }
    }
    return false;
}

void Kernel::wake_timed_out_threads() {
    for (const auto& thread : threads_) {
        if (thread->status != ThreadStatus::waiting || !thread->wait.deadline ||
            *thread->wait.deadline > clock.cycles()) {
            continue;
        }
        switch (thread->wait.kind) {
        case WaitKind::sleep: wake(*thread, k_ok); break;
        case WaitKind::cond: finish_cond_wait(*thread, k_error_timed_out); break;
        default: wake(*thread, k_error_timed_out);
        }
    }
}

bool Kernel::skip_to_next_timeout(std::optional<u64> device_deadline) {
    std::optional<u64> next;
    auto consider = [&](u64 deadline) { next = std::min(next.value_or(deadline), deadline); };
    if (device_deadline) consider(*device_deadline);
    for (const auto& thread : threads_) {
        if (thread->status == ThreadStatus::waiting && thread->wait.deadline) {
            consider(*thread->wait.deadline);
        }
    }
    // A stalled SPU thread may be waiting for its DMA or its decrementer.
    for (const auto& thread : spu_threads_) {
        if (thread->status != ThreadStatus::waiting || !thread->blocked_channel) continue;
        if (const auto deadline = thread->spe->channels.next_deadline(*thread->blocked_channel)) {
            consider(*deadline);
        }
    }
    if (!next) return false;
    if (*next > clock.cycles()) clock.advance(*next - clock.cycles());
    return true;
}

std::string Kernel::describe_waits() const {
    std::string text;
    for (const auto& thread : threads_) {
        if (thread->status != ThreadStatus::waiting) continue;
        text += std::format("\n  thread {:#x} \"{}\" waits on {} {:#x}", thread->id, thread->name,
                            wait_kind_name(thread->wait.kind), thread->wait.object);
    }
    for (const auto& thread : spu_threads_) {
        if (thread->status != ThreadStatus::waiting) continue;
        if (thread->waiting_queue != 0) {
            text += std::format("\n  SPU thread {:#x} \"{}\" waits on event queue {:#x}",
                                thread->id, thread->name, thread->waiting_queue);
            continue;
        }
        const u32 channel = *thread->blocked_channel;
        text += std::format("\n  SPU thread {:#x} \"{}\" waits on channel {} ({})", thread->id,
                            thread->name, channel, spu::channel_name(channel).value_or("?"));
    }
    return text;
}

// ---- Objects ----

u32 Kernel::add_object(std::unique_ptr<KernelObject> object) {
    const u32 id = next_id_++;
    objects_[id] = std::move(object);
    return id;
}

void Kernel::remove_object(u32 id) {
    objects_.erase(id);
}

} // namespace lv2
