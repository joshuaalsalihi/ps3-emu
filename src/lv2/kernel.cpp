#include "lv2/kernel.h"

#include "common/log.h"

#include <format>
#include <ostream>
#include <vector>

namespace lv2 {

namespace {

// Syscalls return a 32-bit int, sign-extended into the 64-bit register.
void set_result(ppu::Thread& thread, s32 result) {
    thread.gpr(3) = static_cast<u64>(s64{result});
}

// Pointer arguments are 32-bit guest addresses.
u32 pointer_arg(ppu::Thread& thread, u32 index) {
    return static_cast<u32>(thread.gpr(3 + index));
}

} // namespace

Kernel::Kernel(std::ostream& tty) : tty_(tty) {}

void Kernel::handle_syscall(ppu::Thread& thread) {
    const u64 number = thread.gpr(11);
    switch (number) {
    case k_sys_process_exit: exit_status_ = static_cast<s32>(thread.gpr(3)); return;
    case k_sys_time_get_timebase_frequency:
        thread.gpr(3) = VirtualClock::k_timebase_frequency;
        return;
    case k_sys_tty_write:
        set_result(thread,
                   sys_tty_write(thread.memory, pointer_arg(thread, 0), pointer_arg(thread, 1),
                                 pointer_arg(thread, 2), pointer_arg(thread, 3)));
        return;
    default:
        log_warning("lv2",
                    std::format("unimplemented syscall {} at {:#x}", number, thread.state.pc - 4));
        set_result(thread, k_error_not_implemented);
    }
}

// int sys_tty_write(u32 ch, const void* buf, u32 len, u32* pwritelen)
s32 Kernel::sys_tty_write(Memory& memory, u32 /*channel*/, u32 buffer, u32 length,
                          u32 written_out) {
    // Validate before allocating, so a bad length can't make us allocate gigabytes.
    if (!memory.is_accessible(buffer, length, Memory::k_read)) return k_error_bad_address;
    if (written_out != 0 && !memory.is_accessible(written_out, 4, Memory::k_write)) {
        return k_error_bad_address;
    }

    std::vector<u8> bytes(length);
    memory.read_bytes(buffer, bytes);
    if (written_out != 0) memory.write<u32>(written_out, length);
    tty_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(length));
    return k_ok;
}

} // namespace lv2
