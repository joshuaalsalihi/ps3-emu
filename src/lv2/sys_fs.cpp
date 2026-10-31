// Filesystem syscalls. The virtual filesystem comes in a later milestone; until then no file
// can be opened, so every file descriptor is invalid.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

// CellFsErrno sys_fs_fstat(int fd, CellFsStat* stat)
// newlib calls this on stdout to decide how to buffer it. On a PS3, the console isn't a file
// opened through sys_fs either, so this matches what LV2 reports.
void sys_fs_fstat(Kernel&, PpuThread& thread) {
    thread.set_result(k_error_bad_file_descriptor);
}

constexpr SyscallInfo k_syscalls[] = {
    {809, "sys_fs_fstat", sys_fs_fstat},
};

} // namespace

std::span<const SyscallInfo> fs_syscalls() {
    return k_syscalls;
}

} // namespace lv2
