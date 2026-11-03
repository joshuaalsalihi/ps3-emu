// libgcm's system functions and video output (cellGcmSys, cellSysutil), called directly with an
// RSX attached to the kernel.

#include "hle/linker.h"
#include "rsx/rsx.h"

#include "support/kernel_test.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string_view>
#include <utility>

using namespace lv2;
using rsx::Rsx;
using test::KernelTest;

namespace {

constexpr u32 k_io = 0x1000'0000;
constexpr u32 k_io_size = Rsx::k_io_page_size;
constexpr u32 k_command_size = 0x1'0000;
constexpr u64 k_out = KernelTest::k_data; // output arguments go here

class GcmTest : public KernelTest {
public:
    GcmTest() {
        memory.map(k_io, k_io_size, Memory::k_read_write);
        kernel.rsx = &rsx;
    }

    // Calls the HLE function `name` from cellGcmSys or cellSysutil with `args` in r3, r4, ...
    void call(std::string_view name, std::initializer_list<u64> args) {
        u32 reg = 3;
        for (const u64 arg : args) {
            ppu.cpu.state.gpr[reg++] = arg;
        }
        for (const auto functions : {hle::cell_gcm_sys_functions(), hle::cell_sysutil_functions(),
                                     hle::internal_functions()}) {
            for (const hle::FunctionInfo& function : functions) {
                if (function.name == name) return function.function(kernel, ppu);
            }
        }
        FAIL("no HLE function " << name);
    }

    s32 result() const { return KernelTest::result(ppu); }

    // Initializes libgcm, returning the context's address.
    u32 init() {
        call("_cellGcmInitBody", {k_out, k_command_size, k_io_size, k_io});
        REQUIRE(result() == k_ok);
        return memory.read<u32>(k_out);
    }

    u32 context_current(u32 context) const { return memory.read<u32>(context + 8); }

    Rsx rsx{memory, clock};
    PpuThread& ppu = new_thread();
};

} // namespace

// ---- Initialization ----

TEST_CASE_METHOD(GcmTest, "init sets up the command buffer 4 KB into the IO area") {
    const u32 context = init();
    CHECK(context == Rsx::k_context);
    CHECK(memory.read<u32>(context) == k_io + 0x1000);                 // begin
    CHECK(memory.read<u32>(context + 4) == k_io + k_command_size - 4); // end
    CHECK(context_current(context) == k_io + 0x1000);
    CHECK(memory.read<u32>(context + 12) ==
          hle::Linker::internal_descriptor(hle::InternalFunction::gcm_command_buffer_full));
    CHECK(rsx.put() == 0x1000);
    CHECK(rsx.get() == 0x1000);
    CHECK(rsx.address_to_offset(k_io + 0x40) == 0x40);
}

TEST_CASE_METHOD(GcmTest, "init checks its sizes and alignment") {
    call("_cellGcmInitBody", {k_out, k_io_size + 1, k_io_size, k_io});
    CHECK(result() != k_ok);
    call("_cellGcmInitBody", {k_out, k_command_size, k_io_size, k_io + 0x1000});
    CHECK(result() != k_ok);
    CHECK_FALSE(rsx.is_initialized());
}

TEST_CASE_METHOD(GcmTest, "the configuration describes local and IO memory") {
    init();
    call("cellGcmGetConfiguration", {k_out});
    CHECK(result() == k_ok);
    CHECK(memory.read<u32>(k_out) == Rsx::k_local_base);
    CHECK(memory.read<u32>(k_out + 4) == k_io);
    CHECK(memory.read<u32>(k_out + 8) == Rsx::k_local_available);
    CHECK(memory.read<u32>(k_out + 12) == k_io_size);
}

TEST_CASE_METHOD(GcmTest, "the control register and labels have fixed addresses") {
    call("cellGcmGetControlRegister", {});
    CHECK(ppu.cpu.state.gpr[3] == Rsx::k_control_register);
    call("cellGcmGetLabelAddress", {3});
    CHECK(ppu.cpu.state.gpr[3] == Rsx::k_labels + 48);
}

// ---- Addresses ----

TEST_CASE_METHOD(GcmTest, "addresses and offsets convert both ways") {
    init();
    call("cellGcmAddressToOffset", {Rsx::k_local_base + 0x500, k_out});
    CHECK(result() == k_ok);
    CHECK(memory.read<u32>(k_out) == 0x500);
    call("cellGcmAddressToOffset", {0x2000'0000, k_out});
    CHECK(result() != k_ok);

    call("cellGcmIoOffsetToAddress", {0x80, k_out});
    CHECK(result() == k_ok);
    CHECK(memory.read<u32>(k_out) == k_io + 0x80);
}

TEST_CASE_METHOD(GcmTest, "main memory can be mapped for the RSX") {
    init();
    memory.map(0x2000'0000, Rsx::k_io_page_size, Memory::k_read_write);
    call("cellGcmMapMainMemory", {0x2000'0000, Rsx::k_io_page_size, k_out});
    CHECK(result() == k_ok);
    CHECK(memory.read<u32>(k_out) == Rsx::k_io_page_size);
}

// ---- Flips ----

TEST_CASE_METHOD(GcmTest, "a flip and a wait-flip go into the command buffer") {
    const u32 context = init();
    call("cellGcmSetDisplayBuffer", {1, 0, 64, 1, 1});
    CHECK(result() == k_ok);
    call("cellGcmSetFlip", {context, 1});
    CHECK(result() == k_ok);
    call("cellGcmSetWaitFlip", {context});
    CHECK(context_current(context) == k_io + 0x1000 + 16);

    memory.write<u32>(Rsx::k_control_register, 0x1000 + 16); // PUT, as rsxFlushBuffer does
    rsx.run();
    CHECK(rsx.describe_wait() == "RSX waits for a flip");
    CHECK(rsx.next_deadline());
}

TEST_CASE_METHOD(GcmTest, "the flip status goes to 1 on reset and back to 0 after a flip") {
    const u32 context = init();
    call("cellGcmSetFlipMode", {1}); // hsync
    call("cellGcmResetFlipStatus", {});
    call("cellGcmGetFlipStatus", {});
    CHECK(ppu.cpu.state.gpr[3] == 1);

    call("cellGcmSetFlip", {context, 0});
    memory.write<u32>(Rsx::k_control_register, 0x1000 + 8);
    rsx.run();
    call("cellGcmGetFlipStatus", {});
    CHECK(ppu.cpu.state.gpr[3] == 0);
}

TEST_CASE_METHOD(GcmTest, "a display buffer ID must be below 8") {
    init();
    call("cellGcmSetDisplayBuffer", {8, 0, 64, 1, 1});
    CHECK(result() != k_ok);
}

// ---- The command buffer callback ----

TEST_CASE_METHOD(GcmTest, "a full command buffer wraps around once the RSX catches up") {
    const u32 context = init();
    const u32 near_end = k_io + 0x2000;
    memory.write<u32>(context + 8, near_end); // as if commands filled the buffer up to here
    const u64 pc = ppu.cpu.state.pc;

    // First the callback writes a jump back to the start and waits for the RSX to reach it.
    call("cellGcmCallback", {context, 16});
    CHECK(memory.read<u32>(near_end) == (0x1000 | 1));
    CHECK(rsx.put() == 0x2000);
    CHECK(ppu.cpu.state.pc == pc - 4); // it will run again

    ppu.cpu.state.pc = pc;
    rsx.run(); // the RSX runs the (empty) commands up to the jump
    CHECK(rsx.get() == 0x2000);

    call("cellGcmCallback", {context, 16});
    CHECK(result() == k_ok);
    CHECK(ppu.cpu.state.pc == pc);
    CHECK(context_current(context) == k_io + 0x1000);
    CHECK(rsx.put() == 0x1000);

    rsx.run(); // takes the jump
    CHECK(rsx.get() == 0x1000);
    CHECK_FALSE(rsx.error());
}

TEST_CASE_METHOD(GcmTest, "a flip that doesn't fit waits for the buffer to wrap") {
    const u32 context = init();
    memory.write<u32>(context + 8, k_io + k_command_size - 8); // 4 bytes left before `end`
    const u64 pc = ppu.cpu.state.pc;
    call("cellGcmSetFlip", {context, 0});
    CHECK(ppu.cpu.state.pc == pc - 4);
}

// ---- Video output ----

TEST_CASE_METHOD(GcmTest, "the display is an enabled 720p screen") {
    call("cellVideoOutGetState", {0, 0, k_out});
    CHECK(result() == k_ok);
    CHECK(memory.read<u8>(k_out) == 1);      // enabled
    CHECK(memory.read<u8>(k_out + 8) == 2);  // VIDEO_RESOLUTION_720
    CHECK(memory.read<u8>(k_out + 11) == 2); // 16:9

    call("cellVideoOutGetResolution", {2, k_out});
    CHECK(result() == k_ok);
    CHECK(memory.read<u16>(k_out) == 1280);
    CHECK(memory.read<u16>(k_out + 2) == 720);
    call("cellVideoOutGetResolution", {3, k_out});
    CHECK(result() != k_ok);
}

TEST_CASE_METHOD(GcmTest, "a video configuration is kept and reported back") {
    memory.write<u8>(k_out, 1); // 1080p
    memory.write<u8>(k_out + 1, 0);
    memory.write<u8>(k_out + 2, 2);
    memory.write<u32>(k_out + 12, 1920 * 4);
    call("cellVideoOutConfigure", {0, k_out, 0, 0});
    CHECK(result() == k_ok);
    CHECK(rsx.video_mode.resolution == 1);

    call("cellVideoOutGetConfiguration", {0, k_out + 0x40, 0});
    CHECK(memory.read<u8>(k_out + 0x40) == 1);
    CHECK(memory.read<u32>(k_out + 0x40 + 12) == 1920 * 4);

    memory.write<u8>(k_out, 3); // no such resolution
    call("cellVideoOutConfigure", {0, k_out, 0, 0});
    CHECK(result() != k_ok);
}

TEST_CASE_METHOD(GcmTest, "the display accepts 720p, 480p and 576p") {
    for (const auto& [id, available] : {std::pair{1, 0}, {2, 1}, {4, 1}, {5, 1}, {13, 0}}) {
        call("cellVideoOutGetResolutionAvailability", {0, static_cast<u64>(id), 0, 0});
        CHECK(result() == available);
    }
    call("cellVideoOutGetResolutionAvailability", {1, 2, 0, 0}); // not the primary output
    CHECK(result() == 0);
}

TEST_CASE_METHOD(GcmTest, "an automatic aspect becomes the display's 16:9") {
    memory.write<u8>(k_out, 2);     // 720p
    memory.write<u8>(k_out + 2, 0); // VIDEO_ASPECT_AUTO
    call("cellVideoOutConfigure", {0, k_out, 0, 0});
    CHECK(result() == k_ok);
    CHECK(rsx.video_mode.aspect == 2); // VIDEO_ASPECT_16_9
}
