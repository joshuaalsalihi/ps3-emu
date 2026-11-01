// SPU thread and SPU thread group syscalls. Numbers and signatures: psdevwiki, "LV2 Functions
// and Syscalls"; structure layouts: PSL1GHT, sys/spu.h and lv2/spu.h (with 32-bit pointers).

#include "lv2/kernel.h"

#include <algorithm>

namespace lv2 {

namespace {

constexpr u32 k_max_name_length = 127;
constexpr u32 k_segment_size = 16; // sys_spu_segment_t

// Reads a name given as {pointer, length including the terminator}.
std::string read_name(Kernel& kernel, u32 address, u32 size) {
    if (address == 0 || size == 0) return "";
    return kernel.memory.read_string(address, std::min(size, k_max_name_length));
}

u32 id_arg(const PpuThread& thread, u32 index) {
    return static_cast<u32>(thread.arg(index));
}

// sys_spu_image_t: {u32 type, u32 entry_point, u32 segments, u32 segment_count}.
SpuImage read_image(Kernel& kernel, u32 address) {
    SpuImage image;
    image.entry = kernel.memory.read<u32>(address + 4);
    const u32 segments = kernel.memory.read<u32>(address + 8);
    const u32 count = kernel.memory.read<u32>(address + 12);
    for (u32 i = 0; i < count; ++i) {
        const u32 segment = segments + i * k_segment_size;
        image.segments.push_back(
            {kernel.memory.read<u32>(segment), kernel.memory.read<u32>(segment + 4),
             kernel.memory.read<u32>(segment + 8), kernel.memory.read<u32>(segment + 12)});
    }
    return image;
}

// int sys_spu_initialize(u32 max_usable_spu, u32 max_raw_spu)
void sys_spu_initialize(Kernel&, PpuThread& thread) {
    const bool valid = thread.arg(0) <= Kernel::k_spu_count && thread.arg(1) <= thread.arg(0);
    thread.set_result(valid ? k_ok : k_error_invalid);
}

// int sys_spu_thread_group_create(sys_spu_thread_group_t* id, u32 num, int prio,
//                                 sys_spu_thread_group_attribute_t* attr)
// attr: {u32 name_size, u32 name, u32 type, u32 container}.
void sys_spu_thread_group_create(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const u32 attr = thread.pointer_arg(3);
    if (attr == 0) return thread.set_result(k_error_bad_address);
    const std::string name =
        read_name(kernel, kernel.memory.read<u32>(attr + 4), kernel.memory.read<u32>(attr));
    if (!kernel.memory.is_accessible(id_out, 4, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }

    u32 id = 0;
    const s32 result = kernel.create_spu_thread_group(static_cast<u32>(thread.arg(1)),
                                                      static_cast<s32>(thread.arg(2)), name, id);
    if (result == k_ok) kernel.memory.write<u32>(id_out, id);
    thread.set_result(result);
}

// int sys_spu_thread_group_destroy(sys_spu_thread_group_t id)
void sys_spu_thread_group_destroy(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.destroy_spu_thread_group(id_arg(thread, 0)));
}

// int sys_spu_thread_initialize(sys_spu_thread_t* thread, sys_spu_thread_group_t group,
//                               u32 spu_num, sys_spu_image_t* img,
//                               sys_spu_thread_attribute_t* attr, sys_spu_thread_argument_t* arg)
// attr: {u32 name, u32 name_size, u32 option}; arg: {u64 arg1, arg2, arg3, arg4}.
void sys_spu_thread_initialize(Kernel& kernel, PpuThread& thread) {
    const u32 id_out = thread.pointer_arg(0);
    const u32 attr = thread.pointer_arg(4);
    const u32 arg = thread.pointer_arg(5);
    if (thread.pointer_arg(3) == 0 || attr == 0) return thread.set_result(k_error_bad_address);

    const SpuImage image = read_image(kernel, thread.pointer_arg(3));
    const std::string name =
        read_name(kernel, kernel.memory.read<u32>(attr), kernel.memory.read<u32>(attr + 4));
    std::array<u64, 4> arguments{};
    for (u32 i = 0; arg != 0 && i < 4; ++i) {
        arguments[i] = kernel.memory.read<u64>(arg + 8 * i);
    }
    if (!kernel.memory.is_accessible(id_out, 4, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }

    u32 id = 0;
    const s32 result = kernel.initialize_spu_thread(id_arg(thread, 1), id_arg(thread, 2), image,
                                                    name, arguments, id);
    if (result == k_ok) kernel.memory.write<u32>(id_out, id);
    thread.set_result(result);
}

// int sys_spu_thread_set_argument(sys_spu_thread_t id, sys_spu_thread_argument_t* arg)
// The arguments take effect the next time the group starts.
void sys_spu_thread_set_argument(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    std::array<u64, 4> arguments{};
    for (u32 i = 0; i < 4; ++i) {
        arguments[i] = kernel.memory.read<u64>(thread.pointer_arg(1) + 8 * i);
    }
    target->arguments = arguments;
    target->needs_reload = true;
    thread.set_result(k_ok);
}

// int sys_spu_thread_group_start(sys_spu_thread_group_t id)
void sys_spu_thread_group_start(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.start_spu_thread_group(id_arg(thread, 0)));
}

// int sys_spu_thread_group_terminate(sys_spu_thread_group_t id, int value)
void sys_spu_thread_group_terminate(Kernel& kernel, PpuThread& thread) {
    thread.set_result(
        kernel.terminate_spu_thread_group(id_arg(thread, 0), static_cast<s32>(thread.arg(1))));
}

// int sys_spu_thread_group_join(sys_spu_thread_group_t gid, int* cause, int* status)
void sys_spu_thread_group_join(Kernel& kernel, PpuThread& thread) {
    const auto result = kernel.join_spu_thread_group(thread, id_arg(thread, 0),
                                                     thread.pointer_arg(1), thread.pointer_arg(2));
    if (result) thread.set_result(*result);
}

// int sys_spu_thread_group_set_priority(sys_spu_thread_group_t id, int priority)
void sys_spu_thread_group_set_priority(Kernel& kernel, PpuThread& thread) {
    SpuThreadGroup* group = kernel.find_object<SpuThreadGroup>(id_arg(thread, 0));
    if (!group) return thread.set_result(k_error_no_such_id);
    const s32 priority = static_cast<s32>(thread.arg(1));
    if (priority < Kernel::k_min_spu_priority || priority > Kernel::k_max_spu_priority) {
        return thread.set_result(k_error_invalid);
    }
    group->priority = priority; // UNVERIFIED: priorities don't affect our scheduling
    thread.set_result(k_ok);
}

// int sys_spu_thread_group_get_priority(sys_spu_thread_group_t id, int* priority)
void sys_spu_thread_group_get_priority(Kernel& kernel, PpuThread& thread) {
    SpuThreadGroup* group = kernel.find_object<SpuThreadGroup>(id_arg(thread, 0));
    if (!group) return thread.set_result(k_error_no_such_id);
    kernel.memory.write<u32>(thread.pointer_arg(1), static_cast<u32>(group->priority));
    thread.set_result(k_ok);
}

// int sys_spu_thread_get_exit_status(sys_spu_thread_t id, int* status)
void sys_spu_thread_get_exit_status(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    if (!target->exit_status) return thread.set_result(k_error_bad_state);
    kernel.memory.write<u32>(thread.pointer_arg(1), static_cast<u32>(*target->exit_status));
    thread.set_result(k_ok);
}

// Local store accesses of 1, 2, 4 or 8 bytes, naturally aligned.
bool valid_local_access(u32 address, u64 size) {
    const bool valid_size = size == 1 || size == 2 || size == 4 || size == 8;
    return valid_size && address % size == 0 && address < spu::k_local_store_size;
}

// int sys_spu_thread_write_ls(sys_spu_thread_t id, u32 address, u64 value, size_t type)
void sys_spu_thread_write_ls(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    const u32 address = static_cast<u32>(thread.arg(1));
    const u64 value = thread.arg(2);
    const u64 size = thread.arg(3);
    if (!target) return thread.set_result(k_error_no_such_id);
    if (!valid_local_access(address, size)) return thread.set_result(k_error_invalid);

    spu::LocalStore& local = target->spe->cpu.local_store;
    switch (size) {
    case 1: local.write<u8>(address, static_cast<u8>(value)); break;
    case 2: local.write<u16>(address, static_cast<u16>(value)); break;
    case 4: local.write<u32>(address, static_cast<u32>(value)); break;
    default: local.write<u64>(address, value); break;
    }
    thread.set_result(k_ok);
}

// int sys_spu_thread_read_ls(sys_spu_thread_t id, u32 address, u64* value, size_t type)
void sys_spu_thread_read_ls(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    const u32 address = static_cast<u32>(thread.arg(1));
    const u64 size = thread.arg(3);
    if (!target) return thread.set_result(k_error_no_such_id);
    if (!valid_local_access(address, size)) return thread.set_result(k_error_invalid);

    const spu::LocalStore& local = target->spe->cpu.local_store;
    u64 value = 0;
    switch (size) {
    case 1: value = local.read<u8>(address); break;
    case 2: value = local.read<u16>(address); break;
    case 4: value = local.read<u32>(address); break;
    default: value = local.read<u64>(address); break;
    }
    kernel.memory.write<u64>(thread.pointer_arg(2), value);
    thread.set_result(k_ok);
}

// int sys_spu_thread_write_snr(sys_spu_thread_t id, int number, u32 value)
// `number` is 0 or 1, for signal notification registers 1 and 2.
void sys_spu_thread_write_snr(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    if (thread.arg(1) > 1) return thread.set_result(k_error_invalid);
    target->spe->channels.write_signal(static_cast<u32>(thread.arg(1)),
                                       static_cast<u32>(thread.arg(2)));
    thread.set_result(k_ok);
}

// int sys_spu_thread_write_spu_mb(sys_spu_thread_t id, u32 value)
// UNVERIFIED: a full inbound mailbox gives EBUSY. The CBEA leaves overrunning it undefined.
void sys_spu_thread_write_spu_mb(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    const bool written = target->spe->channels.write_in_mbox(static_cast<u32>(thread.arg(1)));
    thread.set_result(written ? k_ok : k_error_busy);
}

// int sys_spu_thread_set_spu_cfg(sys_spu_thread_t id, u64 value)
// Bit 0 puts signal 1 in OR mode, bit 1 signal 2. PSL1GHT, sys/spu.h: SPU_SIGNAL1_OR.
void sys_spu_thread_set_spu_cfg(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    const u64 value = thread.arg(1);
    if (!target) return thread.set_result(k_error_no_such_id);
    if (value > 3) return thread.set_result(k_error_invalid);
    target->spe->channels.signal_or_mode[0] = (value & 1) != 0;
    target->spe->channels.signal_or_mode[1] = (value & 2) != 0;
    thread.set_result(k_ok);
}

// int sys_spu_thread_get_spu_cfg(sys_spu_thread_t id, u64* value)
void sys_spu_thread_get_spu_cfg(Kernel& kernel, PpuThread& thread) {
    SpuThread* target = kernel.find_spu_thread(id_arg(thread, 0));
    if (!target) return thread.set_result(k_error_no_such_id);
    const auto& channels = target->spe->channels;
    const u64 value = (channels.signal_or_mode[0] ? 1 : 0) | (channels.signal_or_mode[1] ? 2 : 0);
    kernel.memory.write<u64>(thread.pointer_arg(1), value);
    thread.set_result(k_ok);
}

// int sys_spu_thread_connect_event(sys_spu_thread_t id, sys_event_queue_t eq,
//                                  sys_event_type_t et, uint8_t spup)
void sys_spu_thread_connect_event(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.connect_spu_thread_event(id_arg(thread, 0), id_arg(thread, 1),
                                                      id_arg(thread, 2), thread.arg(3) & 0xff));
}

// int sys_spu_thread_disconnect_event(sys_spu_thread_t id, sys_event_type_t et, uint8_t spup)
void sys_spu_thread_disconnect_event(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.disconnect_spu_thread_event(id_arg(thread, 0), id_arg(thread, 1),
                                                         thread.arg(2) & 0xff));
}

// int sys_spu_thread_bind_queue(sys_spu_thread_t id, sys_event_queue_t spuq, uint32_t spuq_num)
void sys_spu_thread_bind_queue(Kernel& kernel, PpuThread& thread) {
    thread.set_result(
        kernel.bind_spu_thread_queue(id_arg(thread, 0), id_arg(thread, 1), id_arg(thread, 2)));
}

// int sys_spu_thread_unbind_queue(sys_spu_thread_t id, uint32_t spuq_num)
void sys_spu_thread_unbind_queue(Kernel& kernel, PpuThread& thread) {
    thread.set_result(kernel.unbind_spu_thread_queue(id_arg(thread, 0), id_arg(thread, 1)));
}

constexpr SyscallInfo k_syscalls[] = {
    {165, "sys_spu_thread_get_exit_status", sys_spu_thread_get_exit_status},
    {166, "sys_spu_thread_set_argument", sys_spu_thread_set_argument},
    {169, "sys_spu_initialize", sys_spu_initialize},
    {170, "sys_spu_thread_group_create", sys_spu_thread_group_create},
    {171, "sys_spu_thread_group_destroy", sys_spu_thread_group_destroy},
    {172, "sys_spu_thread_initialize", sys_spu_thread_initialize},
    {173, "sys_spu_thread_group_start", sys_spu_thread_group_start},
    {177, "sys_spu_thread_group_terminate", sys_spu_thread_group_terminate},
    {178, "sys_spu_thread_group_join", sys_spu_thread_group_join},
    {179, "sys_spu_thread_group_set_priority", sys_spu_thread_group_set_priority},
    {180, "sys_spu_thread_group_get_priority", sys_spu_thread_group_get_priority},
    {181, "sys_spu_thread_write_ls", sys_spu_thread_write_ls},
    {182, "sys_spu_thread_read_ls", sys_spu_thread_read_ls},
    {184, "sys_spu_thread_write_snr", sys_spu_thread_write_snr},
    {187, "sys_spu_thread_set_spu_cfg", sys_spu_thread_set_spu_cfg},
    {188, "sys_spu_thread_get_spu_cfg", sys_spu_thread_get_spu_cfg},
    {190, "sys_spu_thread_write_spu_mb", sys_spu_thread_write_spu_mb},
    {191, "sys_spu_thread_connect_event", sys_spu_thread_connect_event},
    {192, "sys_spu_thread_disconnect_event", sys_spu_thread_disconnect_event},
    {193, "sys_spu_thread_bind_queue", sys_spu_thread_bind_queue},
    {194, "sys_spu_thread_unbind_queue", sys_spu_thread_unbind_queue},
};

} // namespace

std::span<const SyscallInfo> spu_syscalls() {
    return k_syscalls;
}

} // namespace lv2
