// cellGcmSys: libgcm's system part, which sets up the RSX for a program: memory maps, the command
// buffer, display buffers and flips. Programs build most commands themselves (PSL1GHT's librsx
// does); these functions only write the few that need the system's help.
//
// Signatures: PSL1GHT's rsx/gcm_sys.h, whose gcm* functions wrap these (with 32-bit pointers).

#include "hle/linker.h"
#include "rsx/methods.h"
#include "rsx/rsx.h"

namespace hle {

namespace {

using lv2::Kernel;
using lv2::PpuThread;
using rsx::Rsx;

// UNVERIFIED: libgcm has its own error codes (CELL_GCM_ERROR_*), which we don't have yet.
constexpr s32 k_gcm_error = lv2::k_error_invalid;

// The command buffer starts 4 KB into the IO area: PSL1GHT's rsxResetCommandBuffer jumps back to
// IO offset 0x1000.
constexpr u32 k_command_buffer_start = 0x1000;

// gcmContextData: {u32 begin, u32 end, u32 current, u32 callback}.
constexpr u32 k_context_begin = 0;
constexpr u32 k_context_end = 4;
constexpr u32 k_context_current = 8;
constexpr u32 k_context_callback = 12;

constexpr u32 k_jump_command = 1; // low bits of a jump: envytools, NV4-style pusher

// Runs the HLE call again on the thread's next turn, by moving pc back onto its `hle`.
void retry_later(PpuThread& thread) {
    thread.cpu.state.pc -= 4;
}

// A method command header: `count` words for `method` on `subchannel`.
constexpr u32 method_header(u32 subchannel, u32 method, u32 count) {
    return (count << 18) | (subchannel << 13) | method;
}

// The guest's own view of where the RSX's FIFO is.
void write_put(Kernel& kernel, u32 offset) {
    kernel.memory.write<u32>(Rsx::k_control_register, offset);
}

// Makes room for `words` more words in the command buffer by wrapping around to its start, as
// libgcm's default callback does: a jump back to the start, then a wait for the RSX to reach it,
// so nothing it still has to read gets overwritten. Returns false if the thread must try again
// later.
bool wrap_command_buffer(Kernel& kernel, u32 context) {
    Memory& memory = kernel.memory;
    const u32 begin = memory.read<u32>(context + k_context_begin);
    const u32 current = memory.read<u32>(context + k_context_current);
    const u32 begin_offset = *kernel.rsx->address_to_offset(begin);
    const auto jump_offset = kernel.rsx->address_to_offset(current);
    if (!jump_offset) return true; // not our buffer: nothing we can do

    // First the jump, and PUT up to it, so the RSX finishes everything before.
    const u32 jump = begin_offset | k_jump_command;
    if (memory.read<u32>(current) != jump || kernel.rsx->put() != *jump_offset) {
        memory.write<u32>(current, jump);
        write_put(kernel, *jump_offset);
        return false;
    }
    if (kernel.rsx->get() != *jump_offset) return false;

    // The RSX is at the jump: it will take it, and find new commands from the start.
    write_put(kernel, begin_offset);
    memory.write<u32>(context + k_context_current, begin);
    return true;
}

// Appends commands to the context's buffer. Returns false if the thread must try again later.
bool push(Kernel& kernel, u32 context, std::initializer_list<u32> words) {
    Memory& memory = kernel.memory;
    const u32 end = memory.read<u32>(context + k_context_end);
    if (memory.read<u32>(context + k_context_current) + 4 * words.size() > end &&
        !wrap_command_buffer(kernel, context)) {
        return false;
    }
    u32 current = memory.read<u32>(context + k_context_current);
    for (const u32 word : words) {
        memory.write<u32>(current, word);
        current += 4;
    }
    memory.write<u32>(context + k_context_current, current);
    return true;
}

bool has_rsx(Kernel& kernel) {
    return kernel.rsx && kernel.rsx->is_initialized();
}

// ---- Initialization ----

// s32 _cellGcmInitBody(gcmContextData** context, u32 cmdSize, u32 ioSize, void* ioAddress)
// Maps the IO buffer at IO offset 0 and makes its first `cmdSize` bytes, after the reserved
// 4 KB, the default command buffer.
void cell_gcm_init_body(Kernel& kernel, PpuThread& thread) {
    const u32 context_out = thread.pointer_arg(0);
    const u32 command_size = static_cast<u32>(thread.arg(1));
    const u32 io_size = static_cast<u32>(thread.arg(2));
    const u32 io_address = thread.pointer_arg(3);
    if (!kernel.rsx || command_size <= k_command_buffer_start || command_size > io_size ||
        !kernel.rsx->initialize(io_address, io_size)) {
        return thread.set_result(k_gcm_error);
    }

    // UNVERIFIED: the last word stays free, for the jump that wraps around.
    const u32 context = Rsx::k_context;
    kernel.memory.write<u32>(context + k_context_begin, io_address + k_command_buffer_start);
    kernel.memory.write<u32>(context + k_context_end, io_address + command_size - 4);
    kernel.memory.write<u32>(context + k_context_current, io_address + k_command_buffer_start);
    kernel.memory.write<u32>(
        context + k_context_callback,
        Linker::internal_descriptor(InternalFunction::gcm_command_buffer_full));
    write_put(kernel, k_command_buffer_start);
    kernel.memory.write<u32>(Rsx::k_control_register + 4, k_command_buffer_start); // GET
    kernel.memory.write<u32>(context_out, context);
    thread.set_result(lv2::k_ok);
}

// s32 cellGcmCallback(gcmContextData* context, u32 count): the default command buffer's
// callback, called when `count` more words don't fit.
void cell_gcm_command_buffer_full(Kernel& kernel, PpuThread& thread) {
    if (!wrap_command_buffer(kernel, thread.pointer_arg(0))) return retry_later(thread);
    thread.set_result(lv2::k_ok);
}

// s32 cellGcmGetConfiguration(gcmConfiguration* config)
// {u32 localAddress, u32 ioAddress, u32 localSize, u32 ioSize, u32 memoryFreq, u32 coreFreq}
void cell_gcm_get_configuration(Kernel& kernel, PpuThread& thread) {
    if (!has_rsx(kernel)) return thread.set_result(k_gcm_error);
    const u32 config = thread.pointer_arg(0);
    kernel.memory.write<u32>(config, Rsx::k_local_base);
    kernel.memory.write<u32>(config + 4, *kernel.rsx->io_offset_to_address(0));
    kernel.memory.write<u32>(config + 8, Rsx::k_local_available);
    kernel.memory.write<u32>(config + 12, kernel.rsx->io_size());
    // The RSX's published clocks: 650 MHz memory, 500 MHz core.
    kernel.memory.write<u32>(config + 16, 650'000'000);
    kernel.memory.write<u32>(config + 20, 500'000'000);
    thread.set_result(lv2::k_ok);
}

// u32* cellGcmGetControlRegister(void)
void cell_gcm_get_control_register(Kernel&, PpuThread& thread) {
    thread.cpu.state.gpr[3] = Rsx::k_control_register;
}

// u32* cellGcmGetLabelAddress(u8 index)
void cell_gcm_get_label_address(Kernel&, PpuThread& thread) {
    thread.cpu.state.gpr[3] = Rsx::k_labels + 16 * (thread.arg(0) & 0xff);
}

// ---- Addresses ----

// s32 cellGcmAddressToOffset(const void* address, u32* offset)
void cell_gcm_address_to_offset(Kernel& kernel, PpuThread& thread) {
    if (!has_rsx(kernel)) return thread.set_result(k_gcm_error);
    const auto offset = kernel.rsx->address_to_offset(thread.pointer_arg(0));
    if (!offset) return thread.set_result(k_gcm_error);
    kernel.memory.write<u32>(thread.pointer_arg(1), *offset);
    thread.set_result(lv2::k_ok);
}

// s32 cellGcmIoOffsetToAddress(u32 offset, void** address)
void cell_gcm_io_offset_to_address(Kernel& kernel, PpuThread& thread) {
    if (!has_rsx(kernel)) return thread.set_result(k_gcm_error);
    const auto address = kernel.rsx->io_offset_to_address(static_cast<u32>(thread.arg(0)));
    if (!address) return thread.set_result(k_gcm_error);
    kernel.memory.write<u32>(thread.pointer_arg(1), *address);
    thread.set_result(lv2::k_ok);
}

// s32 cellGcmMapMainMemory(const void* address, u32 size, u32* offset)
void cell_gcm_map_main_memory(Kernel& kernel, PpuThread& thread) {
    if (!has_rsx(kernel)) return thread.set_result(k_gcm_error);
    const auto offset =
        kernel.rsx->map_main_memory(thread.pointer_arg(0), static_cast<u32>(thread.arg(1)));
    if (!offset) return thread.set_result(k_gcm_error);
    kernel.memory.write<u32>(thread.pointer_arg(2), *offset);
    thread.set_result(lv2::k_ok);
}

// ---- Display ----

// s32 cellGcmSetDisplayBuffer(u8 id, u32 offset, u32 pitch, u32 width, u32 height)
void cell_gcm_set_display_buffer(Kernel& kernel, PpuThread& thread) {
    const u32 id = static_cast<u32>(thread.arg(0));
    if (!has_rsx(kernel) || id >= Rsx::k_display_buffer_count) {
        return thread.set_result(k_gcm_error);
    }
    kernel.rsx->set_display_buffer(
        id, {static_cast<u32>(thread.arg(1)), static_cast<u32>(thread.arg(2)),
             static_cast<u32>(thread.arg(3)), static_cast<u32>(thread.arg(4))});
    thread.set_result(lv2::k_ok);
}

// void cellGcmSetFlipMode(u32 mode)
void cell_gcm_set_flip_mode(Kernel& kernel, PpuThread& thread) {
    const u32 mode = static_cast<u32>(thread.arg(0));
    // UNVERIFIED: the third mode (HSYNC_AND_BREAK_EVERYTHING) flips like HSYNC.
    if (kernel.rsx) {
        kernel.rsx->set_flip_mode(mode == 2 ? rsx::FlipMode::vsync : rsx::FlipMode::hsync);
    }
}

// u32 cellGcmGetFlipStatus(void)
void cell_gcm_get_flip_status(Kernel& kernel, PpuThread& thread) {
    thread.cpu.state.gpr[3] = kernel.rsx ? kernel.rsx->flip_status() : 0;
}

// void cellGcmResetFlipStatus(void)
void cell_gcm_reset_flip_status(Kernel& kernel, PpuThread&) {
    if (kernel.rsx) kernel.rsx->reset_flip_status();
}

// s32 cellGcmSetFlip(gcmContextData* context, u8 id), and _cellGcmSetFlipCommand: queues a flip
// to display buffer `id` once the RSX reaches it.
void cell_gcm_set_flip(Kernel& kernel, PpuThread& thread) {
    if (!has_rsx(kernel)) return thread.set_result(k_gcm_error);
    const u32 id = static_cast<u32>(thread.arg(1) & 0xff);
    const u32 header = method_header(rsx::method::k_hle_subchannel, rsx::method::k_hle_flip, 1);
    if (!push(kernel, thread.pointer_arg(0), {header, id})) return retry_later(thread);
    thread.set_result(lv2::k_ok);
}

// void cellGcmSetWaitFlip(gcmContextData* context): the RSX waits until the queued flip is done,
// so it doesn't draw into a buffer still on screen.
void cell_gcm_set_wait_flip(Kernel& kernel, PpuThread& thread) {
    if (!has_rsx(kernel)) return;
    const u32 header =
        method_header(rsx::method::k_hle_subchannel, rsx::method::k_hle_wait_flip, 1);
    if (!push(kernel, thread.pointer_arg(0), {header, 0})) return retry_later(thread);
}

constexpr FunctionInfo k_functions[] = {
    {"_cellGcmInitBody", cell_gcm_init_body},
    {"cellGcmGetConfiguration", cell_gcm_get_configuration},
    {"cellGcmGetControlRegister", cell_gcm_get_control_register},
    {"cellGcmGetLabelAddress", cell_gcm_get_label_address},
    {"cellGcmAddressToOffset", cell_gcm_address_to_offset},
    {"cellGcmIoOffsetToAddress", cell_gcm_io_offset_to_address},
    {"cellGcmMapMainMemory", cell_gcm_map_main_memory},
    {"cellGcmSetDisplayBuffer", cell_gcm_set_display_buffer},
    {"cellGcmSetFlipMode", cell_gcm_set_flip_mode},
    {"cellGcmGetFlipStatus", cell_gcm_get_flip_status},
    {"cellGcmResetFlipStatus", cell_gcm_reset_flip_status},
    {"cellGcmSetFlip", cell_gcm_set_flip},
    {"_cellGcmSetFlipCommand", cell_gcm_set_flip},
    {"cellGcmSetWaitFlip", cell_gcm_set_wait_flip},
};

constexpr FunctionInfo k_internal_functions[] = {
    {"cellGcmCallback", cell_gcm_command_buffer_full}, // InternalFunction::gcm_command_buffer_full
};

} // namespace

std::span<const FunctionInfo> cell_gcm_sys_functions() {
    return k_functions;
}

std::span<const FunctionInfo> internal_functions() {
    return k_internal_functions;
}

} // namespace hle
