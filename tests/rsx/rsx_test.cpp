// The RSX: its FIFO, state model, clears and flips. Commands are written into IO memory as
// libgcm would, then the RSX runs them.

#include "rsx/methods.h"
#include "rsx/rsx.h"

#include "support/rsx_test.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace rsx;
using test::RsxTest;

namespace {

constexpr u32 header(u32 method, u32 count, u32 subchannel = 0) {
    return count << 18 | subchannel << 13 | method;
}

} // namespace

// ---- FIFO ----

TEST_CASE_METHOD(RsxTest, "methods are written to consecutive registers") {
    methods(0x1000, {1, 2, 3});
    rsx.run();
    CHECK(rsx.reg(0x1000) == 1);
    CHECK(rsx.reg(0x1004) == 2);
    CHECK(rsx.reg(0x1008) == 3);
    CHECK(rsx.get() == rsx.put());
}

TEST_CASE_METHOD(RsxTest, "non-increasing methods write every value to the same register") {
    push({0x4000'0000 | header(0x1000, 3), 1, 2, 3});
    rsx.run();
    CHECK(rsx.reg(0x1000) == 3);
    CHECK(rsx.reg(0x1004) == 0);
}

TEST_CASE_METHOD(RsxTest, "each subchannel has its own registers") {
    methods(0x1000, {5}, 3);
    rsx.run();
    CHECK(rsx.reg(0x1000, 3) == 5);
    CHECK(rsx.reg(0x1000, 0) == 0);
}

TEST_CASE_METHOD(RsxTest, "jumps move GET, in both encodings") {
    push({0x2000 | 1}); // jump to 0x2000
    memory.write<u32>(k_io + 0x2000, header(0x1000, 1));
    memory.write<u32>(k_io + 0x2004, 7);
    memory.write<u32>(k_io + 0x2008, 0x2000'3000); // old-style jump to 0x3000
    memory.write<u32>(k_io + 0x3000, header(0x1004, 1));
    memory.write<u32>(k_io + 0x3004, 8);
    set_put(0x3008);
    rsx.run();
    CHECK(rsx.reg(0x1000) == 7);
    CHECK(rsx.reg(0x1004) == 8);
    CHECK(rsx.get() == 0x3008);
}

TEST_CASE_METHOD(RsxTest, "a call runs a subroutine and returns after it") {
    push({0x2000 | 2}); // call 0x2000
    memory.write<u32>(k_io + 0x2000, header(0x1000, 1));
    memory.write<u32>(k_io + 0x2004, 9);
    memory.write<u32>(k_io + 0x2008, 0x0002'0000); // return
    methods(0x1004, {10});
    rsx.run();
    CHECK(rsx.reg(0x1000) == 9);
    CHECK(rsx.reg(0x1004) == 10);
    CHECK(rsx.get() == rsx.put());
}

TEST_CASE_METHOD(RsxTest, "a return without a call is an error") {
    push({0x0002'0000});
    rsx.run();
    REQUIRE(rsx.error());
    CHECK(rsx.error()->find("return") != std::string::npos);
}

TEST_CASE_METHOD(RsxTest, "an invalid command is an error") {
    push({0x8000'0000});
    rsx.run();
    CHECK(rsx.error());
}

TEST_CASE_METHOD(RsxTest, "nothing happens until PUT moves") {
    memory.write<u32>(k_io + k_start, header(0x1000, 1));
    memory.write<u32>(k_io + k_start + 4, 1);
    rsx.run();
    CHECK(rsx.reg(0x1000) == 0);
    CHECK_FALSE(rsx.has_work());
}

// ---- Labels and the reference value ----

TEST_CASE_METHOD(RsxTest, "SET_REFERENCE writes the control register's REF") {
    CHECK(memory.read<u32>(Rsx::k_control_register + 8) == 0xffff'ffff);
    methods(method::k_set_reference, {42});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_control_register + 8) == 42);
}

TEST_CASE_METHOD(RsxTest, "a semaphore release writes a label") {
    methods(method::k_semaphore_offset, {0x30});
    methods(method::k_semaphore_release, {0x1234});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_labels + 0x30) == 0x1234);
}

TEST_CASE_METHOD(RsxTest, "a semaphore acquire waits until the label has the value") {
    methods(method::k_semaphore_offset, {0x10});
    methods(method::k_semaphore_acquire, {5});
    methods(0x1000, {1});
    rsx.run();
    CHECK(rsx.reg(0x1000) == 0);
    CHECK_FALSE(rsx.has_work());
    CHECK(rsx.describe_wait() == "RSX waits for label 1 to be 0x5");

    memory.write<u32>(Rsx::k_labels + 0x10, 5);
    CHECK(rsx.has_work());
    rsx.run();
    CHECK(rsx.reg(0x1000) == 1);
}

TEST_CASE_METHOD(RsxTest, "the 3D class's releases write labels") {
    // The back end's value has bytes 0 and 2 swapped, as libgcm sends it: 1 arrives as 0x10000.
    methods(method::k_semaphore_offset_3d, {0x20});
    methods(method::k_back_end_write_semaphore_release, {0x0001'0000});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_labels + 0x20) == 1);
    methods(method::k_back_end_write_semaphore_release, {0x1122'3344});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_labels + 0x20) == 0x1144'3322);

    methods(method::k_texture_read_semaphore_release, {0x1122'3344});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_labels + 0x20) == 0x1122'3344);
}

// ---- Addresses ----

TEST_CASE_METHOD(RsxTest, "addresses become local or IO offsets") {
    CHECK(rsx.address_to_offset(Rsx::k_local_base + 0x100) == 0x100);
    CHECK(rsx.address_to_offset(k_io + 0x20) == 0x20);
    CHECK_FALSE(rsx.address_to_offset(0x2000'0000));
    CHECK(rsx.io_offset_to_address(0x20) == k_io + 0x20);
    CHECK_FALSE(rsx.io_offset_to_address(Rsx::k_io_page_size));
}

TEST_CASE_METHOD(RsxTest, "more main memory can be mapped into IO space") {
    memory.map(0x2000'0000, 2 * Rsx::k_io_page_size, Memory::k_read_write);
    CHECK_FALSE(rsx.map_main_memory(0x2000'0100, Rsx::k_io_page_size)); // not 1 MB aligned
    const auto offset = rsx.map_main_memory(0x2000'0000, 2 * Rsx::k_io_page_size);
    CHECK(offset == Rsx::k_io_page_size); // after the IO buffer
    CHECK(rsx.address_to_offset(0x2010'0004) == 2 * Rsx::k_io_page_size + 4);
}

// ---- Clears ----

TEST_CASE_METHOD(RsxTest, "a clear fills the render target with the clear color") {
    set_surface();
    methods(method::k_color_clear_value, {0x8012'3456});
    methods(method::k_clear_surface, {0xf0});
    rsx.run();
    CHECK(pixel(0, 0) == 0xff12'3456); // X8R8G8B8_O8R8G8B8: the X byte reads as 1s
    CHECK(pixel(15, 7) == 0xff12'3456);
    CHECK(pixel(16, 0) == 0); // outside the surface
    CHECK(pixel(0, 8) == 0);
}

TEST_CASE_METHOD(RsxTest, "an A8R8G8B8 clear writes alpha too") {
    set_surface(8);
    methods(method::k_color_clear_value, {0x8012'3456});
    methods(method::k_clear_surface, {0xf0});
    rsx.run();
    CHECK(pixel(3, 3) == 0x8012'3456);
}

TEST_CASE_METHOD(RsxTest, "a clear changes only the channels in its mask") {
    set_surface(8);
    memory.write<u32>(Rsx::k_local_base, 0x1122'3344);
    methods(method::k_color_clear_value, {0xaabb'ccdd});
    methods(method::k_clear_surface, {method::k_clear_red | method::k_clear_blue});
    rsx.run();
    CHECK(pixel(0, 0) == 0x11bb'33dd);
}

TEST_CASE_METHOD(RsxTest, "a clear stays inside the scissor") {
    set_surface(8);
    methods(method::k_scissor_horizontal, {2 | 3u << 16, 1 | 2u << 16}); // x 2-4, y 1-2
    methods(method::k_color_clear_value, {0xffff'ffff});
    methods(method::k_clear_surface, {0xf0});
    rsx.run();
    CHECK(pixel(2, 1) == 0xffff'ffff);
    CHECK(pixel(4, 2) == 0xffff'ffff);
    CHECK(pixel(1, 1) == 0);
    CHECK(pixel(5, 1) == 0);
    CHECK(pixel(2, 3) == 0);
}

TEST_CASE_METHOD(RsxTest, "depth and stencil clear separately") {
    set_surface(8 | 2 << 5); // Z24S8
    methods(method::k_set_zeta_context, {method::k_context_local});
    methods(method::k_surface_zeta_offset, {0x1000});
    methods(method::k_surface_pitch_zeta, {64});
    methods(method::k_zstencil_clear_value, {0xabcd'ef12});
    methods(method::k_clear_surface, {method::k_clear_depth});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_local_base + 0x1000) == 0xabcd'ef00);
    methods(method::k_clear_surface, {method::k_clear_stencil});
    rsx.run();
    CHECK(memory.read<u32>(Rsx::k_local_base + 0x1000) == 0xabcd'ef12);
}

TEST_CASE_METHOD(RsxTest, "an unsupported surface format is an error") {
    set_surface(3); // R5G6B5
    methods(method::k_clear_surface, {0xf0});
    rsx.run();
    REQUIRE(rsx.error());
    CHECK(rsx.error()->find("format 3 is not supported yet") != std::string::npos);
}

// ---- Flips ----

constexpr u32 k_flip = header(method::k_hle_flip, 1, method::k_hle_subchannel);
constexpr u32 k_wait_flip = header(method::k_hle_wait_flip, 1, method::k_hle_subchannel);

TEST_CASE_METHOD(RsxTest, "a vsync flip shows its buffer at the next vblank") {
    rsx.set_display_buffer(1, {0, 64, 2, 1});
    memory.write<u32>(Rsx::k_local_base, 0x00ff'0000);
    memory.write<u32>(Rsx::k_local_base + 4, 0x0000'00ff);
    rsx.reset_flip_status();
    push({k_flip, 1});
    rsx.run();
    CHECK(rsx.flip_status() == 1);
    CHECK(frames.empty());

    const u64 vblank = *rsx.next_deadline();
    CHECK(vblank == VirtualClock::k_cpu_frequency / Rsx::k_vblank_frequency + 1);
    clock.advance(vblank - 1);
    rsx.run();
    CHECK(frames.empty());
    clock.advance(1);
    rsx.run();
    CHECK(rsx.flip_status() == 0);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].number == 1);
    CHECK(frames[0].rgb == std::vector<u8>{0xff, 0, 0, 0, 0, 0xff});
}

TEST_CASE_METHOD(RsxTest, "an hsync flip shows its buffer straight away") {
    rsx.set_flip_mode(FlipMode::hsync);
    rsx.set_display_buffer(0, {0, 64, 1, 1});
    push({k_flip, 0});
    rsx.run();
    CHECK(frames.size() == 1);
    CHECK_FALSE(rsx.next_deadline());
}

TEST_CASE_METHOD(RsxTest, "wait-flip holds later commands until the flip is done") {
    rsx.set_display_buffer(0, {0, 64, 1, 1});
    push({k_flip, 0, k_wait_flip, 0});
    methods(0x1000, {1});
    rsx.run();
    CHECK(rsx.reg(0x1000) == 0);
    CHECK(rsx.describe_wait() == "RSX waits for a flip");

    clock.advance(*rsx.next_deadline());
    rsx.run();
    CHECK(rsx.reg(0x1000) == 1);
}

TEST_CASE_METHOD(RsxTest, "a flip to a display buffer that doesn't exist is an error") {
    push({k_flip, 8});
    rsx.run();
    CHECK(rsx.error());
}

TEST_CASE_METHOD(RsxTest, "frames are counted without a callback, and shown only once") {
    rsx.set_flip_mode(FlipMode::hsync);
    rsx.set_display_buffer(0, {0, 64, 1, 1});
    push({k_flip, 0});
    // The RSX's state, with memory, which holds the FIFO and its GET pointer.
    auto save = [&] {
        state::Archive archive;
        memory.serialize(archive);
        rsx.serialize(archive);
        return archive.take_bytes();
    };
    auto load = [&](const std::vector<u8>& bytes) {
        state::Archive archive(bytes);
        memory.serialize(archive);
        rsx.serialize(archive);
    };
    const std::vector<u8> start = save();

    // Frames count whether or not anyone watches, so a save state doesn't depend on it.
    rsx.on_frame = nullptr;
    rsx.run();
    const std::vector<u8> unwatched = save();

    rsx.on_frame = [this](const Frame& frame) { frames.push_back(frame); };
    load(start);
    rsx.run();
    CHECK(save() == unwatched);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].number == 1);

    // Going back and replaying the flip doesn't show the frame twice.
    load(start);
    rsx.run();
    CHECK(frames.size() == 1);
}

// ---- Transfers ----

TEST_CASE_METHOD(RsxTest, "an image from the CPU writes its pixels into the 2D surface") {
    // As libgcm's rsxInlineTransfer sends them: a Y32 surface at local offset 0x100 with a
    // 0x40-byte pitch, then a 3x1 image at (2, 1), whose fourth word is padding.
    methods(method::k_surfaces_2d_destination_context, {method::k_context_local},
            method::k_subchannel_surfaces_2d);
    methods(method::k_surfaces_2d_format, {method::k_surface_2d_y32, 0x40u << 16 | 0x40, 0, 0x100},
            method::k_subchannel_surfaces_2d);
    methods(method::k_image_point, {2 | 1u << 16, 3 | 1u << 16, 3 | 1u << 16},
            method::k_subchannel_image_from_cpu);
    methods(method::k_image_color, {0x11, 0x22, 0x33, 0x44}, method::k_subchannel_image_from_cpu);
    rsx.run();
    REQUIRE_FALSE(rsx.error());
    const u32 row = Rsx::k_local_base + 0x100 + 0x40;
    CHECK(memory.read<u32>(row + 4) == 0);
    CHECK(memory.read<u32>(row + 8) == 0x11);
    CHECK(memory.read<u32>(row + 12) == 0x22);
    CHECK(memory.read<u32>(row + 16) == 0x33);
    CHECK(memory.read<u32>(row + 20) == 0); // the padding word goes nowhere
}

TEST_CASE_METHOD(RsxTest, "methods on subchannels we don't model are reported") {
    methods(0x0300, {1}, 2);
    rsx.run();
    REQUIRE(rsx.error());
    CHECK(*rsx.error() == "RSX: methods on subchannel 2 are not supported yet");
}
