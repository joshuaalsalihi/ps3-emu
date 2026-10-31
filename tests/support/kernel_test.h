#pragma once

// Test fixture for the LV2 kernel: a kernel with no program loaded, plus helpers to create
// threads and make syscalls from them.

#include "core/clock.h"
#include "core/memory.h"
#include "lv2/kernel.h"

#include <initializer_list>
#include <sstream>
#include <string_view>

namespace test {

class KernelTest {
public:
    static constexpr u64 k_data = 0x20000; // a readable and writable page for arguments

    KernelTest() { memory.map(k_data, Memory::k_page_size, Memory::k_read_write); }

    // A started thread. Its code is never run; tests drive it through syscalls.
    lv2::PpuThread& new_thread(s32 priority = 1000) {
        lv2::PpuThread* thread = kernel.create_thread("test", 0x10000, 0, 0, priority, 0x4000);
        kernel.start_thread(*thread);
        return *thread;
    }

    // Makes syscall `number` from `thread`, with `args` in r3, r4, ...
    void syscall(lv2::PpuThread& thread, u32 number, std::initializer_list<u64> args = {}) {
        thread.cpu.state.gpr[11] = number;
        u32 reg = 3;
        for (const u64 arg : args) {
            thread.cpu.state.gpr[reg++] = arg;
        }
        kernel.handle_syscall(thread);
    }

    // The 32-bit result a syscall left in r3.
    static s32 result(const lv2::PpuThread& thread) {
        return static_cast<s32>(thread.cpu.state.gpr[3]);
    }

    static bool is_waiting(const lv2::PpuThread& thread) {
        return thread.status == lv2::ThreadStatus::waiting;
    }

    void write_string(u64 address, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            memory.write<u8>(address + i, static_cast<u8>(text[i]));
        }
        memory.write<u8>(address + text.size(), 0);
    }

    VirtualClock clock;
    Memory memory;
    std::ostringstream tty;
    lv2::Kernel kernel{memory, clock, tty};
};

} // namespace test
