// SPU thread groups and SPU threads.
//
// A group is created empty, its threads are initialized one by one (each loads an SPU image into
// its local store), then the whole group starts. It ends when every thread exits, when one
// exits the whole group, or when the PPU terminates it; a PPU thread collects the outcome with
// sys_spu_thread_group_join. A group that has ended can be started again: its threads restart
// from their freshly loaded images.

#include "lv2/kernel.h"

#include <algorithm>

namespace lv2 {

namespace {

// UNVERIFIED: the stack pointer LV2 gives an SPU thread: the top of local store, keeping the
// ABI's 16-byte back chain. The SPU runtime's _start sets $1 itself anyway.
constexpr u32 k_spu_stack_top = spu::k_local_store_size - 16;

} // namespace

// ---- Groups ----

s32 Kernel::create_spu_thread_group(u32 size, s32 priority, std::string name, u32& group_id) {
    if (size == 0 || size > k_spu_count) return k_error_invalid;
    if (priority < k_min_spu_priority || priority > k_max_spu_priority) return k_error_invalid;

    auto group = std::make_unique<SpuThreadGroup>();
    group->name = std::move(name);
    group->size = size;
    group->priority = priority;
    group->threads.resize(size, 0);
    group_id = add_object(std::move(group));
    return k_ok;
}

s32 Kernel::destroy_spu_thread_group(u32 group_id) {
    SpuThreadGroup* group = find_object<SpuThreadGroup>(group_id);
    if (!group) return k_error_no_such_id;
    if (group->status == SpuThreadGroup::Status::running || group->has_joiner) {
        return k_error_busy;
    }

    std::erase_if(spu_threads_, [&](const auto& thread) { return thread->group == group_id; });
    remove_object(group_id);
    return k_ok;
}

s32 Kernel::initialize_spu_thread(u32 group_id, u32 index, const SpuImage& image, std::string name,
                                  const std::array<u64, 4>& arguments, u32& thread_id) {
    SpuThreadGroup* group = find_object<SpuThreadGroup>(group_id);
    if (!group) return k_error_no_such_id;
    if (index >= group->size) return k_error_invalid;
    if (group->threads[index] != 0) return k_error_busy;
    if (group->status == SpuThreadGroup::Status::running) return k_error_bad_state;
    if (image.entry >= spu::k_local_store_size) return k_error_invalid;

    auto thread = std::make_unique<SpuThread>();
    thread->id = next_id_++;
    thread->name = std::move(name);
    thread->group = group_id;
    thread->index = index;
    thread->entry = image.entry;
    thread->arguments = arguments;

    // Build the image in a scratch local store, checking every segment before keeping it.
    std::vector<u8> local(spu::k_local_store_size);
    for (const SpuSegment& segment : image.segments) {
        if (u64{segment.local_address} + segment.size > local.size()) return k_error_invalid;
        const auto destination = std::span(local).subspan(segment.local_address, segment.size);
        switch (segment.type) {
        case k_spu_segment_copy: memory.read_bytes(segment.source_or_value, destination); break;
        case k_spu_segment_fill:
            for (u32 i = 0; i < segment.size; ++i) {
                destination[i] = static_cast<u8>(segment.source_or_value >> (24 - 8 * (i % 4)));
            }
            break;
        case k_spu_segment_info: break;
        default: return k_error_invalid;
        }
    }
    thread->initial_local_store = std::move(local);

    thread_id = thread->id;
    group->threads[index] = thread->id;
    load_spu_thread(*thread);
    spu_threads_.push_back(std::move(thread));

    const bool complete = std::ranges::none_of(group->threads, [](u32 id) { return id == 0; });
    if (complete) group->status = SpuThreadGroup::Status::initialized;
    return k_ok;
}

// Gives the thread a fresh SPE holding its image, ready to start at the entry point.
void Kernel::load_spu_thread(SpuThread& thread) {
    thread.spe = std::make_unique<spu::Spe>(thread.id, memory, clock);
    thread.spe->dma.instant = instant_dma;
    std::ranges::copy(thread.initial_local_store, thread.spe->cpu.local_store.bytes().begin());

    // UNVERIFIED: LV2 passes the four arguments in $3-$6, each in the preferred doubleword, as
    // PSL1GHT's SPU runtime expects.
    spu::State& state = thread.spe->cpu.state;
    state.pc = thread.entry;
    state.gpr[1] = Vector::from_words(k_spu_stack_top, 0, 0, 0);
    for (u32 i = 0; i < 4; ++i) {
        state.gpr[3 + i] = Vector::from_dwords(thread.arguments[i], 0);
    }
    thread.needs_reload = false;
    thread.exit_status.reset();
    thread.blocked_channel.reset();
    thread.waiting_queue = 0;
}

s32 Kernel::start_spu_thread_group(u32 group_id) {
    SpuThreadGroup* group = find_object<SpuThreadGroup>(group_id);
    if (!group) return k_error_no_such_id;
    if (group->status != SpuThreadGroup::Status::initialized) return k_error_bad_state;

    for (const u32 id : group->threads) {
        SpuThread& thread = *find_spu_thread(id);
        if (thread.needs_reload) load_spu_thread(thread);
        thread.needs_reload = true; // the next start begins from the image again
        thread.status = ThreadStatus::ready;
    }
    group->status = SpuThreadGroup::Status::running;
    group->exit.reset();
    return k_ok;
}

s32 Kernel::terminate_spu_thread_group(u32 group_id, s32 value) {
    SpuThreadGroup* group = find_object<SpuThreadGroup>(group_id);
    if (!group) return k_error_no_such_id;
    if (group->status != SpuThreadGroup::Status::running) return k_error_bad_state;

    end_spu_thread_group(*group, group_id, k_spu_join_terminated, value);
    return k_ok;
}

std::optional<s32> Kernel::join_spu_thread_group(PpuThread& thread, u32 group_id, u32 cause_address,
                                                 u32 status_address) {
    SpuThreadGroup* group = find_object<SpuThreadGroup>(group_id);
    if (!group) return k_error_no_such_id;
    if (group->has_joiner) return k_error_busy;

    if (group->exit) {
        if (cause_address != 0) memory.write<u32>(cause_address, group->exit->cause);
        if (status_address != 0) memory.write<u32>(status_address, group->exit->status);
        group->exit.reset();
        return k_ok;
    }
    if (group->status != SpuThreadGroup::Status::running) return k_error_bad_state;

    Wait join = Wait::on(WaitKind::spu_group, group_id);
    join.output_address = cause_address;
    join.status_address = status_address;
    group->has_joiner = true;
    wait(thread, join, 0);
    return std::nullopt;
}

// Stops every thread of the group and hands the outcome to its joiner, or keeps it for one.
void Kernel::end_spu_thread_group(SpuThreadGroup& group, u32 group_id, u32 cause, s32 status) {
    for (const u32 id : group.threads) {
        find_spu_thread(id)->status = ThreadStatus::finished;
    }
    group.status = SpuThreadGroup::Status::initialized;
    group.exit = SpuThreadGroup::Exit{cause, status};

    if (PpuThread* joiner = first_waiter(WaitKind::spu_group, group_id, false)) {
        group.has_joiner = false;
        join_spu_thread_group(*joiner, group_id, joiner->wait.output_address,
                              joiner->wait.status_address);
        wake(*joiner, k_ok);
    }
}

SpuThread* Kernel::find_spu_thread(u32 id) {
    for (const auto& thread : spu_threads_) {
        if (thread->id == id) return thread.get();
    }
    return nullptr;
}

// ---- Running ----

bool Kernel::handle_spu_stop(SpuThread& thread, u32 signal) {
    // UNVERIFIED: an empty outbound mailbox gives an exit status of 0.
    auto exit_status = [&] {
        return static_cast<s32>(thread.spe->channels.read_out_mbox().value_or(0));
    };
    SpuThreadGroup& group = *find_object<SpuThreadGroup>(thread.group);

    switch (signal) {
    case k_spu_stop_group_exit:
        end_spu_thread_group(group, thread.group, k_spu_join_group_exit, exit_status());
        return true;
    case k_spu_stop_thread_exit: {
        thread.exit_status = exit_status();
        thread.status = ThreadStatus::finished;
        const bool all_done = std::ranges::all_of(group.threads, [&](u32 id) {
            return find_spu_thread(id)->status == ThreadStatus::finished;
        });
        // UNVERIFIED: the group's status when all its threads exit on their own.
        if (all_done) end_spu_thread_group(group, thread.group, k_spu_join_all_threads_exit, 0);
        return true;
    }
    case k_spu_stop_yield: return true; // the scheduler moves on after every stop anyway
    case k_spu_stop_receive_event: receive_spu_event(thread, true); return true;
    case k_spu_stop_try_receive_event: receive_spu_event(thread, false); return true;
    default: return false;
    }
}

void Kernel::block_spu_thread(SpuThread& thread, u32 channel) {
    thread.status = ThreadStatus::waiting;
    thread.blocked_channel = channel;
}

// DMA keeps going after a thread stops, so a put issued just before exiting still lands.
void Kernel::update_spu_threads() {
    for (const auto& thread : spu_threads_) {
        thread->spe->channels.update();
        if (thread->status == ThreadStatus::waiting && thread->waiting_queue == 0 &&
            !thread->spe->blocked_channel()) {
            thread->status = ThreadStatus::ready;
            thread->blocked_channel.reset();
        }
    }
}

} // namespace lv2
