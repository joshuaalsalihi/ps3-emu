#pragma once

#include "common/types.h"
#include "cpu/ppu/thread.h"

#include <iosfwd>
#include <optional>

namespace lv2 {

// LV2 error codes, returned in r3. psdevwiki, "Error Codes".
constexpr s32 k_ok = 0;
constexpr s32 k_error_not_implemented = static_cast<s32>(0x80010003); // ENOSYS
constexpr s32 k_error_bad_address = static_cast<s32>(0x8001000D);     // EFAULT

// Syscall numbers. psdevwiki, "LV2 Functions and Syscalls".
constexpr u64 k_sys_process_exit = 3;
constexpr u64 k_sys_time_get_timebase_frequency = 147;
constexpr u64 k_sys_tty_write = 403;

// High-level emulation of the LV2 kernel. Milestone M1 covers just enough to print text and
// exit; threads and synchronization arrive in M2.
class Kernel {
public:
    // `tty` receives what the guest writes to its debug console.
    explicit Kernel(std::ostream& tty);

    // Handles the `sc` instruction `thread` just executed. The syscall number is in r11,
    // arguments are in r3-r10, and the result goes in r3.
    void handle_syscall(ppu::Thread& thread);

    // Set once the guest calls sys_process_exit.
    std::optional<s32> exit_status() const { return exit_status_; }

private:
    s32 sys_tty_write(Memory& memory, u32 channel, u32 buffer, u32 length, u32 written_out);

    std::ostream& tty_;
    std::optional<s32> exit_status_;
};

} // namespace lv2
