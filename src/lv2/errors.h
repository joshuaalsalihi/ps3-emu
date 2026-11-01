#pragma once

#include "common/types.h"

namespace lv2 {

// LV2 error codes, returned in r3. psdevwiki, "Error Codes".
constexpr s32 k_ok = 0;
constexpr s32 k_error_again = static_cast<s32>(0x80010001);               // EAGAIN
constexpr s32 k_error_invalid = static_cast<s32>(0x80010002);             // EINVAL
constexpr s32 k_error_not_implemented = static_cast<s32>(0x80010003);     // ENOSYS
constexpr s32 k_error_no_memory = static_cast<s32>(0x80010004);           // ENOMEM
constexpr s32 k_error_no_such_id = static_cast<s32>(0x80010005);          // ESRCH
constexpr s32 k_error_deadlock = static_cast<s32>(0x80010008);            // EDEADLK
constexpr s32 k_error_not_permitted = static_cast<s32>(0x80010009);       // EPERM
constexpr s32 k_error_busy = static_cast<s32>(0x8001000A);                // EBUSY
constexpr s32 k_error_timed_out = static_cast<s32>(0x8001000B);           // ETIMEDOUT
constexpr s32 k_error_bad_address = static_cast<s32>(0x8001000D);         // EFAULT
constexpr s32 k_error_bad_state = static_cast<s32>(0x8001000F);           // ESTAT
constexpr s32 k_error_alignment = static_cast<s32>(0x80010010);           // EALIGN
constexpr s32 k_error_canceled = static_cast<s32>(0x80010013);            // ECANCELED
constexpr s32 k_error_exists = static_cast<s32>(0x80010014);              // EEXIST
constexpr s32 k_error_is_connected = static_cast<s32>(0x80010015);        // EISCONN
constexpr s32 k_error_not_connected = static_cast<s32>(0x80010016);       // ENOTCONN
constexpr s32 k_error_bad_file_descriptor = static_cast<s32>(0x8001002A); // EBADF

} // namespace lv2
