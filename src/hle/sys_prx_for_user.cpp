// sysPrxForUser: the core user-mode system library (threads, lightweight locks, memory, time).
// On a real PS3 these are firmware code layered on syscalls; here they call the kernel directly.
//
// Lightweight mutexes and condition variables keep their state in a control block in guest
// memory. We only keep the kernel object's ID there; the object does the real work.

#include "hle/linker.h"
#include "loader/elf.h"
#include "loader/spu_elf.h"

namespace hle {

namespace {

using lv2::Kernel;
using lv2::PpuThread;

// Sets the result, unless the thread is now waiting (it gets its result when it wakes).
void finish(PpuThread& thread, std::optional<s32> result) {
    if (result) thread.set_result(*result);
}

// Runs the HLE call again on the thread's next turn, by moving pc back onto its `hle`.
void retry_later(PpuThread& thread) {
    thread.cpu.state.pc -= 4;
}

// ---- Process ----

// void sys_process_exit(int status)
void sys_process_exit(Kernel& kernel, PpuThread& thread) {
    kernel.exit_process(static_cast<s32>(thread.arg(0)));
}

// ---- Threads ----

// int sys_ppu_thread_create(sys_ppu_thread_t* thread_id, const opd32* entry, u64 arg, int prio,
//                           size_t stacksize, u64 flags, const char* name)
// Unlike the syscall, the library function starts the thread right away.
void sys_ppu_thread_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const std::string name =
        thread.pointer_arg(6) == 0 ? "" : kernel.memory.read_string(thread.pointer_arg(6), 27);
    if (!kernel.memory.is_accessible(id_out, 8, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }

    u32 new_id = 0;
    const s32 result = kernel.create_thread_from_descriptor(
        thread.pointer_arg(1), thread.arg(2), static_cast<s32>(thread.arg(3)), thread.arg(4),
        thread.arg(5), name, new_id);
    if (result != lv2::k_ok) return thread.set_result(result);

    kernel.memory.write<u64>(id_out, new_id);
    kernel.start_thread(*kernel.find_thread(new_id));
    thread.set_result(lv2::k_ok);
}

// void sys_ppu_thread_exit(u64 exit_value)
void sys_ppu_thread_exit(Kernel& kernel, PpuThread& thread) {
    kernel.exit_thread(thread, thread.arg(0));
}

// int sys_ppu_thread_get_id(sys_ppu_thread_t* thread_id)
void sys_ppu_thread_get_id(Kernel& kernel, PpuThread& thread) {
    kernel.memory.write<u64>(thread.pointer_arg(0), thread.id);
    thread.set_result(lv2::k_ok);
}

// ---- Lightweight mutex ----
// Control block: {u64 lock_var, u32 attribute, u32 recursive_count, u32 sleep_queue, u32 pad}.
// Attributes: {u32 protocol, u32 recursive, char name[8]}. PSL1GHT, lv2/mutex.h.

constexpr u64 k_lwmutex_attribute = 8;
constexpr u64 k_lwmutex_sleep_queue = 16; // we keep the kernel mutex ID here

u32 lwmutex_id(Kernel& kernel, u32 control) {
    return kernel.memory.read<u32>(control + k_lwmutex_sleep_queue);
}

// int sys_lwmutex_create(sys_lwmutex_t* lwmutex, const sys_lwmutex_attr_t* attr)
void sys_lwmutex_create(Kernel& kernel, PpuThread& thread) {
    const u32 control = thread.pointer_arg(0);
    const u32 protocol = kernel.memory.read<u32>(thread.pointer_arg(1));
    const u32 recursive = kernel.memory.read<u32>(thread.pointer_arg(1) + 4);

    if (protocol < lv2::k_protocol_fifo || protocol > lv2::k_protocol_priority_inherit ||
        (recursive != lv2::k_mutex_recursive && recursive != lv2::k_mutex_not_recursive)) {
        return thread.set_result(lv2::k_error_invalid);
    }
    if (!kernel.memory.is_accessible(control, 24, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }

    auto mutex = std::make_unique<lv2::Mutex>();
    mutex->recursive = recursive == lv2::k_mutex_recursive;
    mutex->wake_by_priority = protocol != lv2::k_protocol_fifo;
    const u32 id = kernel.add_object(std::move(mutex));

    kernel.memory.write<u64>(control, 0);
    kernel.memory.write<u32>(control + k_lwmutex_attribute, protocol | recursive);
    kernel.memory.write<u32>(control + 12, 0);
    kernel.memory.write<u32>(control + k_lwmutex_sleep_queue, id);
    thread.set_result(lv2::k_ok);
}

// int sys_lwmutex_destroy(sys_lwmutex_t* lwmutex)
void sys_lwmutex_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_mutex(lwmutex_id(kernel, thread.pointer_arg(0))));
}

// int sys_lwmutex_lock(sys_lwmutex_t* lwmutex, usecond_t timeout)
void sys_lwmutex_lock(Kernel& kernel, PpuThread& thread) {
    const u32 id = lwmutex_id(kernel, thread.pointer_arg(0));
    finish(thread, kernel.lock_mutex(thread, id, thread.arg(1)));
}

// int sys_lwmutex_trylock(sys_lwmutex_t* lwmutex)
void sys_lwmutex_trylock(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.try_lock_mutex(thread, lwmutex_id(kernel, thread.pointer_arg(0))));
}

// int sys_lwmutex_unlock(sys_lwmutex_t* lwmutex)
void sys_lwmutex_unlock(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.unlock_mutex(thread, lwmutex_id(kernel, thread.pointer_arg(0))));
}

// ---- Lightweight condition variable ----
// Control block, as the firmware sees it: {u32 lwmutex, u32 lwcond_queue}. We keep the kernel
// condition variable ID in lwcond_queue.

u32 lwcond_id(Kernel& kernel, u32 control) {
    return kernel.memory.read<u32>(control + 4);
}

// int sys_lwcond_create(sys_lwcond_t* lwcond, sys_lwmutex_t* lwmutex, sys_lwcond_attr_t* attr)
void sys_lwcond_create(Kernel& kernel, PpuThread& thread) {
    const u32 control = thread.pointer_arg(0);
    const u32 lwmutex = thread.pointer_arg(1);
    if (!kernel.memory.is_accessible(control, 8, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }

    u32 cond_id = 0;
    const s32 result = kernel.create_cond(lwmutex_id(kernel, lwmutex), cond_id);
    if (result == lv2::k_ok) {
        kernel.memory.write<u32>(control, lwmutex);
        kernel.memory.write<u32>(control + 4, cond_id);
    }
    thread.set_result(result);
}

// int sys_lwcond_destroy(sys_lwcond_t* lwcond)
void sys_lwcond_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_cond(lwcond_id(kernel, thread.pointer_arg(0))));
}

// int sys_lwcond_wait(sys_lwcond_t* lwcond, usecond_t timeout)
void sys_lwcond_wait(Kernel& kernel, PpuThread& thread) {
    const u32 id = lwcond_id(kernel, thread.pointer_arg(0));
    finish(thread, kernel.wait_cond(thread, id, thread.arg(1)));
}

// int sys_lwcond_signal(sys_lwcond_t* lwcond)
void sys_lwcond_signal(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.signal_cond(thread, lwcond_id(kernel, thread.pointer_arg(0))));
}

// int sys_lwcond_signal_all(sys_lwcond_t* lwcond)
void sys_lwcond_signal_all(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.signal_cond_all(thread, lwcond_id(kernel, thread.pointer_arg(0))));
}

// int sys_lwcond_signal_to(sys_lwcond_t* lwcond, sys_ppu_thread_t thread_id)
void sys_lwcond_signal_to(Kernel& kernel, PpuThread& thread) {
    const u32 id = lwcond_id(kernel, thread.pointer_arg(0));
    thread.set_result(kernel.signal_cond_to(thread, id, static_cast<u32>(thread.arg(1))));
}

// ---- Spinlock ----
// A 32-bit word: 0 when free, 1 when held.

// void sys_spinlock_initialize(sys_spinlock_t* lock)
void sys_spinlock_initialize(Kernel& kernel, PpuThread& thread) {
    kernel.memory.write<u32>(thread.pointer_arg(0), 0);
}

// void sys_spinlock_lock(sys_spinlock_t* lock)
void sys_spinlock_lock(Kernel& kernel, PpuThread& thread) {
    const u32 lock = thread.pointer_arg(0);
    if (kernel.memory.read<u32>(lock) != 0) return retry_later(thread); // spin
    kernel.memory.write<u32>(lock, 1);
}

// int sys_spinlock_trylock(sys_spinlock_t* lock)
void sys_spinlock_trylock(Kernel& kernel, PpuThread& thread) {
    const u32 lock = thread.pointer_arg(0);
    if (kernel.memory.read<u32>(lock) != 0) return thread.set_result(lv2::k_error_busy);
    kernel.memory.write<u32>(lock, 1);
    thread.set_result(lv2::k_ok);
}

// void sys_spinlock_unlock(sys_spinlock_t* lock)
void sys_spinlock_unlock(Kernel& kernel, PpuThread& thread) {
    kernel.memory.write<u32>(thread.pointer_arg(0), 0);
}

// ---- Time ----

// system_time_t sys_time_get_system_time(void): microseconds since boot.
void sys_time_get_system_time(Kernel& kernel, PpuThread& thread) {
    thread.cpu.state.gpr[3] = kernel.clock.cycles() / (VirtualClock::k_cpu_frequency / 1'000'000);
}

// ---- Memory mapper ----

// int sys_mmapper_allocate_memory(size_t size, u64 flags, sys_memory_t* mem_id)
void sys_mmapper_allocate_memory(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(2);
    if (!kernel.memory.is_accessible(id_out, 4, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }
    u32 block_id = 0;
    const s32 result = kernel.create_memory_block(thread.arg(0), thread.arg(1), block_id);
    if (result == lv2::k_ok) kernel.memory.write<u32>(id_out, block_id);
    thread.set_result(result);
}

// int sys_mmapper_allocate_memory_from_container(size_t size, sys_memory_container_t container,
//                                                u64 flags, sys_memory_t* mem_id)
// Memory containers aren't modeled yet, so the container is ignored.
void sys_mmapper_allocate_memory_from_container(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(3);
    if (!kernel.memory.is_accessible(id_out, 4, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }
    u32 block_id = 0;
    const s32 result = kernel.create_memory_block(thread.arg(0), thread.arg(2), block_id);
    if (result == lv2::k_ok) kernel.memory.write<u32>(id_out, block_id);
    thread.set_result(result);
}

// int sys_mmapper_free_memory(sys_memory_t mem_id)
void sys_mmapper_free_memory(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_memory_block(static_cast<u32>(thread.arg(0))));
}

// int sys_mmapper_map_memory(sys_addr_t address, sys_memory_t mem_id, u64 flags)
void sys_mmapper_map_memory(Kernel& kernel, PpuThread& thread) {
    thread.set_result(
        kernel.map_memory_block(thread.pointer_arg(0), static_cast<u32>(thread.arg(1))));
}

// int sys_mmapper_unmap_memory(sys_addr_t address, sys_memory_t* mem_id)
void sys_mmapper_unmap_memory(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(1);
    if (!kernel.memory.is_accessible(id_out, 4, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }
    u32 block_id = 0;
    const s32 result = kernel.unmap_memory_block(thread.pointer_arg(0), block_id);
    if (result == lv2::k_ok) kernel.memory.write<u32>(id_out, block_id);
    thread.set_result(result);
}

// ---- SPU images ----
// sys_spu_image_t: {u32 type, u32 entry_point, u32 segments, u32 segment_count}, where segments
// points to sys_spu_segment_t {u32 type, u32 ls_start, u32 size, u32 pa_start or value}.
// PSL1GHT, lv2/spu.h.
//
// UNVERIFIED: the firmware copies the ELF into kernel memory; we build the segment table in user
// memory and point copy segments at the program's own ELF, which it keeps for its lifetime.

constexpr u64 k_segment_table_size = 0x1'0000; // one 64 KB page
constexpr u64 k_page_64k_flag = 0x200;         // PSL1GHT, sys/memory.h

// int sys_spu_image_import(sys_spu_image_t* image, const void* elf, u32 type)
void sys_spu_image_import(Kernel& kernel, PpuThread& thread) {
    const u32 image = thread.pointer_arg(0);
    const u32 elf_address = thread.pointer_arg(1);
    if (!kernel.memory.is_accessible(image, 16, Memory::k_write)) {
        return thread.set_result(lv2::k_error_bad_address);
    }

    loader::SpuElf elf;
    try {
        elf = loader::read_spu_elf(kernel.memory, elf_address);
    } catch (const loader::LoadError&) {
        return thread.set_result(lv2::k_error_invalid);
    }

    std::vector<lv2::SpuSegment> segments;
    for (const loader::SpuElfSegment& s : elf.segments) {
        if (s.file_size != 0) {
            segments.push_back({lv2::k_spu_segment_copy, s.local_address, s.file_size,
                                elf_address + s.file_offset});
        }
        if (s.memory_size > s.file_size) {
            segments.push_back({lv2::k_spu_segment_fill, s.local_address + s.file_size,
                                s.memory_size - s.file_size, 0});
        }
    }

    u32 table = 0;
    const s32 result = kernel.allocate_memory(k_segment_table_size, k_page_64k_flag, table);
    if (result != lv2::k_ok) return thread.set_result(result);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const u32 entry = table + 16 * static_cast<u32>(i);
        kernel.memory.write<u32>(entry, segments[i].type);
        kernel.memory.write<u32>(entry + 4, segments[i].local_address);
        kernel.memory.write<u32>(entry + 8, segments[i].size);
        kernel.memory.write<u32>(entry + 12, segments[i].source_or_value);
    }

    kernel.memory.write<u32>(image, static_cast<u32>(thread.arg(2)));
    kernel.memory.write<u32>(image + 4, elf.entry);
    kernel.memory.write<u32>(image + 8, table);
    kernel.memory.write<u32>(image + 12, static_cast<u32>(segments.size()));
    thread.set_result(lv2::k_ok);
}

// int sys_spu_image_close(sys_spu_image_t* image)
void sys_spu_image_close(Kernel& kernel, PpuThread& thread) {
    const u32 table = kernel.memory.read<u32>(thread.pointer_arg(0) + 8);
    thread.set_result(kernel.free_memory(table));
}

constexpr FunctionInfo k_functions[] = {
    {"sys_process_exit", sys_process_exit},
    {"sys_ppu_thread_create", sys_ppu_thread_create},
    {"sys_ppu_thread_exit", sys_ppu_thread_exit},
    {"sys_ppu_thread_get_id", sys_ppu_thread_get_id},
    {"sys_lwmutex_create", sys_lwmutex_create},
    {"sys_lwmutex_destroy", sys_lwmutex_destroy},
    {"sys_lwmutex_lock", sys_lwmutex_lock},
    {"sys_lwmutex_trylock", sys_lwmutex_trylock},
    {"sys_lwmutex_unlock", sys_lwmutex_unlock},
    {"sys_lwcond_create", sys_lwcond_create},
    {"sys_lwcond_destroy", sys_lwcond_destroy},
    {"sys_lwcond_wait", sys_lwcond_wait},
    {"sys_lwcond_signal", sys_lwcond_signal},
    {"sys_lwcond_signal_all", sys_lwcond_signal_all},
    {"sys_lwcond_signal_to", sys_lwcond_signal_to},
    {"sys_spinlock_initialize", sys_spinlock_initialize},
    {"sys_spinlock_lock", sys_spinlock_lock},
    {"sys_spinlock_trylock", sys_spinlock_trylock},
    {"sys_spinlock_unlock", sys_spinlock_unlock},
    {"sys_time_get_system_time", sys_time_get_system_time},
    {"sys_mmapper_allocate_memory", sys_mmapper_allocate_memory},
    {"sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container},
    {"sys_mmapper_free_memory", sys_mmapper_free_memory},
    {"sys_mmapper_map_memory", sys_mmapper_map_memory},
    {"sys_mmapper_unmap_memory", sys_mmapper_unmap_memory},
    {"sys_spu_image_import", sys_spu_image_import},
    {"sys_spu_image_close", sys_spu_image_close},
};

} // namespace

std::span<const FunctionInfo> sys_prx_for_user_functions() {
    return k_functions;
}

} // namespace hle
