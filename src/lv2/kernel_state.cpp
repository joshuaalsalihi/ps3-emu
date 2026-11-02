// Saving and loading the kernel's state. Threads and objects are recreated when loading, since
// they hold references to memory and the clock.

#include "lv2/kernel.h"

#include "lv2/spu_thread.h"

namespace lv2 {

namespace {

std::unique_ptr<KernelObject> make_object(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::mutex: return std::make_unique<Mutex>();
    case ObjectKind::cond: return std::make_unique<Cond>();
    case ObjectKind::semaphore: return std::make_unique<Semaphore>();
    case ObjectKind::event_queue: return std::make_unique<EventQueue>();
    case ObjectKind::event_port: return std::make_unique<EventPort>();
    case ObjectKind::memory_block: return std::make_unique<MemoryBlock>();
    case ObjectKind::spu_thread_group: return std::make_unique<SpuThreadGroup>();
    }
    throw state::Error("unknown kernel object kind in save state");
}

} // namespace

void Kernel::serialize(state::Archive& archive) {
    // Loading replaces every thread and object.
    if (archive.loading()) {
        threads_.clear();
        spu_threads_.clear();
        objects_.clear();
        mapped_blocks_.clear();
    }

    // PPU threads: their identity first, which creating one needs.
    const u64 thread_count = archive.size(threads_.size());
    for (u64 i = 0; i < thread_count; ++i) {
        u32 id = archive.loading() ? 0 : threads_[i]->id;
        std::string name = archive.loading() ? "" : threads_[i]->name;
        s32 priority = archive.loading() ? 0 : threads_[i]->priority;
        archive(id, name, priority);
        if (archive.loading()) {
            threads_.push_back(std::make_unique<PpuThread>(id, name, priority, memory, clock));
        }
        archive(*threads_[i]);
    }

    // SPU threads, each with its SPE if it has been initialized.
    const u64 spu_thread_count = archive.size(spu_threads_.size());
    for (u64 i = 0; i < spu_thread_count; ++i) {
        if (archive.loading()) spu_threads_.push_back(std::make_unique<SpuThread>());
        SpuThread& thread = *spu_threads_[i];
        archive(thread);
        bool has_spe = thread.spe != nullptr;
        archive(has_spe);
        if (!has_spe) continue;
        if (archive.loading()) thread.spe = std::make_unique<spu::Spe>(thread.id, memory, clock);
        archive(*thread.spe);
    }

    // Objects: the kind of each, then its fields.
    const u64 object_count = archive.size(objects_.size());
    auto it = objects_.begin();
    for (u64 i = 0; i < object_count; ++i) {
        u32 id = archive.loading() ? 0 : it->first;
        ObjectKind kind = archive.loading() ? ObjectKind::mutex : it->second->kind();
        archive(id, kind);
        if (archive.loading()) {
            it = objects_.emplace(id, make_object(kind)).first;
        }
        archive(*it->second);
        ++it;
    }

    archive(next_to_schedule_, tls_address_, tls_file_size_, tls_memory_size_, next_wait_sequence_,
            next_id_, stacks_, user_memory_, mmapper_ranges_, exit_status_, tty_written_);

    // Where memory blocks are mapped within each reserved range. RangeAllocator has no empty
    // state, so these are created as they are read.
    const u64 range_count = archive.size(mapped_blocks_.size());
    auto range = mapped_blocks_.begin();
    for (u64 i = 0; i < range_count; ++i) {
        if (archive.loading()) {
            u64 start = 0;
            archive(start);
            range = mapped_blocks_.emplace(start, RangeAllocator(0, 0)).first;
        } else {
            u64 start = range->first;
            archive(start);
        }
        archive(range->second);
        ++range;
    }
}

} // namespace lv2
