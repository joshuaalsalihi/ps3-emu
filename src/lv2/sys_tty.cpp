// Debug console syscalls.

#include "lv2/kernel.h"

#include <ostream>
#include <vector>

namespace lv2 {

namespace {

// int sys_tty_write(u32 channel, const void* buffer, u32 length, u32* written)
void sys_tty_write(Kernel& kernel, PpuThread& thread) {
    const u32 buffer = thread.pointer_arg(1);
    const u32 length = static_cast<u32>(thread.arg(2));
    const u32 written_out = thread.pointer_arg(3);

    // Check before allocating, so a bad length can't make us allocate gigabytes.
    if (!kernel.memory.is_accessible(buffer, length, Memory::k_read)) {
        return thread.set_result(k_error_bad_address);
    }
    if (written_out != 0 && !kernel.memory.is_accessible(written_out, 4, Memory::k_write)) {
        return thread.set_result(k_error_bad_address);
    }

    std::vector<u8> bytes(length);
    kernel.memory.read_bytes(buffer, bytes);
    if (written_out != 0) kernel.memory.write<u32>(written_out, length);
    kernel.write_tty(bytes);
    thread.set_result(k_ok);
}

constexpr SyscallInfo k_syscalls[] = {
    {403, "sys_tty_write", sys_tty_write},
};

} // namespace

std::span<const SyscallInfo> tty_syscalls() {
    return k_syscalls;
}

} // namespace lv2
