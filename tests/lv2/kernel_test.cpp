#include "lv2/kernel.h"

#include "support/ppu_test.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string_view>

using namespace lv2;

namespace {

// Runs syscalls through the kernel, with guest console output captured in `tty`.
struct KernelTest : test::PpuTest {
    std::ostringstream tty;
    Kernel kernel{tty};

    void syscall(u64 number) {
        r(11) = number;
        kernel.handle_syscall(thread);
    }

    void write_string(u64 address, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            memory.write<u8>(address + i, static_cast<u8>(text[i]));
        }
    }
};

constexpr u64 k_buffer = test::PpuTest::k_data;

} // namespace

TEST_CASE_METHOD(KernelTest, "sys_tty_write prints guest memory and reports its length") {
    write_string(k_buffer, "hello");
    r(3) = 0;
    r(4) = k_buffer;
    r(5) = 5;
    r(6) = k_buffer + 0x100; // where to store the length
    syscall(k_sys_tty_write);

    CHECK(tty.str() == "hello");
    CHECK(r(3) == k_ok);
    CHECK(memory.read<u32>(k_buffer + 0x100) == 5);
}

TEST_CASE_METHOD(KernelTest, "sys_tty_write rejects bad pointers") {
    r(3) = 0;
    r(4) = 0x9000'0000;
    r(5) = 5;
    r(6) = 0;
    syscall(k_sys_tty_write);

    CHECK(tty.str().empty());
    CHECK(r(3) == static_cast<u64>(s64{k_error_bad_address}));
}

TEST_CASE_METHOD(KernelTest, "sys_tty_write rejects lengths past the end of memory") {
    r(3) = 0;
    r(4) = k_buffer;
    r(5) = 0xffff'ffff;
    r(6) = 0;
    syscall(k_sys_tty_write);
    CHECK(r(3) == static_cast<u64>(s64{k_error_bad_address}));
}

TEST_CASE_METHOD(KernelTest, "sys_process_exit records the exit status") {
    CHECK_FALSE(kernel.exit_status());
    r(3) = 42;
    syscall(k_sys_process_exit);
    CHECK(kernel.exit_status() == 42);
}

TEST_CASE_METHOD(KernelTest, "sys_time_get_timebase_frequency") {
    syscall(k_sys_time_get_timebase_frequency);
    CHECK(r(3) == 79'800'000);
}

TEST_CASE_METHOD(KernelTest, "unimplemented syscalls return ENOSYS") {
    syscall(999);
    CHECK(r(3) == 0xffff'ffff'8001'0003); // sign-extended
}
