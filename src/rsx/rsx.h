#pragma once

// The RSX: the PS3's GPU, as programs see it. The PPU writes commands into a buffer in main
// memory and moves the PUT pointer; the RSX reads them from GET up to PUT, writing its registers
// (the state model) and acting on some of them: clears, draws, label writes, flips.
//
// Addresses: the RSX sees its own 256 MB of local memory, and main memory only through the IO
// map, which libgcm sets up in 1 MB pages. Both are named by offsets: a local offset, or an IO
// offset (as the FIFO's GET and PUT are).

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "rsx/backend.h"

#include <array>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace rsx {

// A display buffer, set by cellGcmSetDisplayBuffer. The offset is in local memory.
struct DisplayBuffer {
    u32 offset = 0;
    u32 pitch = 0;
    u32 width = 0;
    u32 height = 0;
};

// The video output mode, from cellVideoOutConfigure. PSL1GHT, sysutil/video.h.
struct VideoMode {
    u8 resolution = 2; // VIDEO_RESOLUTION_720: UNVERIFIED as the virtual display's default
    u8 format = 0;     // VIDEO_BUFFER_FORMAT_XRGB
    u8 aspect = 2;     // VIDEO_ASPECT_16_9
    u32 pitch = 1280 * 4;
};

// cellGcmSetFlipMode. PSL1GHT, rsx/gcm_sys.h.
enum class FlipMode : u32 { hsync = 1, vsync = 2 };

class Rsx {
public:
    // Where the PPU sees local memory. UNVERIFIED: psdevwiki's memory map puts it here.
    static constexpr u32 k_local_base = 0xc000'0000;
    static constexpr u32 k_local_size = 0x1000'0000; // 256 MB
    // The top of local memory holds what libgcm keeps for itself (the control register,
    // labels), so programs get the rest. UNVERIFIED: the real split.
    static constexpr u32 k_local_available = 0x0f90'0000;
    static constexpr u32 k_system_area = k_local_base + k_local_available;
    static constexpr u32 k_control_register = k_system_area; // {put, get, ref}
    static constexpr u32 k_context = k_system_area + 0x100;  // libgcm's gcmContextData
    static constexpr u32 k_labels = k_system_area + 0x1000;  // 256 labels, 16 bytes apart
    static constexpr u32 k_label_count = 256;
    static constexpr u32 k_io_page_size = 0x10'0000; // 1 MB
    static constexpr u32 k_io_page_count = 256;      // 256 MB of IO space
    static constexpr u32 k_display_buffer_count = 8;

    // Commands handled per turn: enough to finish typical bursts, few enough that the PPU keeps
    // running. UNVERIFIED: the real RSX's speed is unknown; this only needs to be fixed.
    static constexpr u64 k_commands_per_turn = 10'000;
    static constexpr u64 k_vblank_frequency = 60; // UNVERIFIED: 59.94 Hz on NTSC displays

    Rsx(Memory& memory, VirtualClock& clock);

    void set_backend(Backend* backend) { backend_ = backend; }
    // If set, every method write is printed here.
    std::ostream* trace = nullptr;
    // Called with every frame that reaches the display.
    std::function<void(const Frame&)> on_frame;

    // ---- What libgcm does (HLE) ----

    // Maps local memory, the system area and `io_size` bytes of main memory at `io_address` to
    // IO offset 0. Returns false if the arguments are invalid.
    bool initialize(u32 io_address, u32 io_size);
    bool is_initialized() const { return initialized_; }
    // Maps more main memory into IO space, at the first free offset.
    std::optional<u32> map_main_memory(u32 address, u32 size);
    // The offset the RSX knows an address by: local memory or mapped main memory.
    std::optional<u32> address_to_offset(u32 address) const;
    std::optional<u32> io_offset_to_address(u32 offset) const;
    u32 io_size() const { return io_size_; } // of the IO buffer mapped at offset 0

    VideoMode video_mode;

    void set_display_buffer(u32 id, const DisplayBuffer& buffer) {
        display_buffers_.at(id) = buffer;
    }
    void set_flip_mode(FlipMode mode) { flip_mode_ = mode; }
    // 0 once the last queued flip has happened; cellGcmResetFlipStatus sets it to 1.
    u32 flip_status() const { return flip_status_; }
    void reset_flip_status() { flip_status_ = 1; }

    // ---- Running ----

    // Handles commands from GET up to PUT, until it runs out, must wait, or has done
    // k_commands_per_turn. Also completes flips whose vblank has come.
    void run();
    // Whether commands are waiting and nothing blocks them.
    bool has_work() const;
    // The next cycle at which the passage of time alone changes something (a vblank a flip
    // waits for), if any.
    std::optional<u64> next_deadline() const;
    // What the RSX waits for, for deadlock reports; empty if it isn't waiting.
    std::string describe_wait() const;
    // A fault the RSX hit (a bad command or address); the run ends with it.
    const std::optional<std::string>& error() const { return error_; }

    // The state model: the last value written to each method of each subchannel.
    u32 reg(u32 method, u32 subchannel = 0) const { return registers_[subchannel][method / 4]; }
    u32 get() const;
    u32 put() const;

private:
    void run_commands();
    void execute(u32 subchannel, u32 method, u32 value);
    void execute_3d(u32 method, u32 value);
    void clear_surface(u32 mask);
    std::optional<ColorSurface> color_surface();
    Rect drawing_area() const;
    void end_primitive();
    void queue_flip(u32 id);
    void complete_flip();
    u64 next_vblank() const;
    // The guest address of `offset` in the memory a DMA context names, if it is mapped.
    std::optional<u32> resolve(u32 context, u32 offset) const;
    void fail(std::string message);

    Memory& memory_;
    VirtualClock& clock_;
    Backend* backend_ = nullptr;
    bool initialized_ = false;
    u32 io_size_ = 0;

    std::array<std::optional<u32>, k_io_page_count> io_pages_; // IO page -> main memory address
    std::array<DisplayBuffer, k_display_buffer_count> display_buffers_{};
    FlipMode flip_mode_ = FlipMode::vsync;
    u32 flip_status_ = 0; // UNVERIFIED: the value before the first reset
    std::optional<u32> queued_flip_;
    u64 flip_queued_at_ = 0;

    // The FIFO: a jump target to return to (one level of call), and why the RSX is stopped.
    std::optional<u32> return_offset_;
    enum class Waiting { nothing, semaphore, flip };
    Waiting waiting_ = Waiting::nothing;
    u32 semaphore_address_ = 0;
    u32 semaphore_value_ = 0;
    u64 frames_ = 0;

    std::array<std::array<u32, 0x800>, 8> registers_{}; // 8 subchannels x 2048 methods

    // Vertex program memory and constants, filled through windows of methods, and the
    // primitive being drawn between BEGIN_END writes.
    std::array<u32, vertex::k_instruction_slots * 4> transform_program_{};
    u32 transform_program_position_ = 0; // in words
    std::array<Vec4, vertex::k_constant_count> transform_constants_{};
    u32 transform_constant_position_ = 0; // in components
    std::optional<u32> primitive_;
    std::vector<u32> draw_vertices_;
    std::optional<std::string> error_;
};

} // namespace rsx
