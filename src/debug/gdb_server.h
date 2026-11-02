#pragma once

// A GDB server: lets GDB debug the guest's PPU threads over the remote serial protocol.
//
// GDB sees each PPU thread as a thread, with the 64-bit PowerPC registers, floating point and
// VMX. It can read and write registers and memory, set breakpoints, step and continue, and stop
// a running program with Ctrl-C. Current GDB releases don't support the SPU, so SPU threads are
// reached through `monitor spu ...` commands instead.
//
// Runs are deterministic, and where a run stops changes nothing the guest sees, so a program
// behaves the same under the debugger as without it. That also lets GDB go back in time
// (reverse-stepi, reverse-continue): the server saves checkpoints as the program runs, and going
// back restores one and runs forward to the right instruction again.
// Reference: the GDB manual, appendix "GDB Remote Serial Protocol".

#include "common/types.h"
#include "core/system.h"
#include "debug/gdb_packet.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace debug {

// A connection to GDB: a TCP socket, or a fake one in tests.
class Connection {
public:
    virtual ~Connection() = default;
    // The next byte from GDB. With `wait`, waits for one, and returns nothing only if the
    // connection has closed; without, returns nothing if no byte is ready.
    virtual std::optional<char> read(bool wait) = 0;
    virtual void write(std::string_view bytes) = 0;
};

class GdbServer {
public:
    // How a debugging session ended.
    enum class End {
        program_ended, // the program exited (see last_result())
        detached,      // GDB detached; the program can go on running without it
        killed,        // GDB killed the program, or the connection closed
    };

    // Runs use `options` (GPU, frames, trace) plus the debugger's breakpoints. Checkpoints for
    // going back are `checkpoint_interval` instructions apart.
    GdbServer(System& system, Connection& connection, RunOptions options = {},
              u64 checkpoint_interval = k_checkpoint_interval);

    // Serves GDB until the session ends.
    End serve();

    // Handles one packet and returns the reply, or nothing if there is none (after `k`).
    // Continuing and stepping run the program; their reply is the stop reply.
    std::optional<std::string> handle(std::string_view packet);

    const RunResult& last_result() const { return last_result_; }
    const std::set<u64>& breakpoints() const { return breakpoints_; }
    std::size_t checkpoint_count() const { return checkpoints_.size(); }

    // How many instructions a run goes before checking whether GDB asked it to stop.
    static constexpr u64 k_poll_interval = 100'000;
    // How far apart checkpoints are, and how many are kept.
    static constexpr u64 k_checkpoint_interval = 1'000'000;
    static constexpr std::size_t k_max_checkpoints = 32;

private:
    std::string query(std::string_view packet);
    RunResult run(RunOptions options, std::optional<u64> end);
    RunResult step(u32 thread, std::optional<u64> end);
    std::string resume(std::optional<u32> step_thread);
    void take_checkpoint();
    void forget_checkpoints();
    std::vector<u64> checkpoints_before(u64 now) const;
    std::string replay_begin();
    std::string reverse_step(u32 thread);
    std::string reverse_continue();
    std::string stop_reply(const RunResult& result);
    std::string monitor(std::string_view command);
    std::string thread_list() const;
    void update_stopped_thread();

    lv2::PpuThread* selected_thread();
    std::string read_registers(const lv2::PpuThread& thread) const;
    bool write_registers(lv2::PpuThread& thread, std::string_view hex);
    std::optional<std::string> read_register(const lv2::PpuThread& thread, u32 number) const;
    bool write_register(lv2::PpuThread& thread, u32 number, std::span<const u8> bytes);
    std::string read_memory(u64 address, u64 length) const;

    System& system_;
    Connection& connection_;
    RunOptions options_;
    u64 checkpoint_interval_;
    gdb::PacketReader reader_;
    bool acknowledging_ = true;
    std::set<u64> breakpoints_;
    u32 selected_thread_ = 0; // for register access (Hg); 0 means the last thread to stop
    u32 stopped_thread_ = 0;  // the thread the last stop reply named
    RunResult last_result_;
    std::map<u64, std::vector<u8>> checkpoints_; // saved states, by instruction count
    std::optional<End> end_;
};

// The target description GDB reads to learn the registers (qXfer:features:read:target.xml).
std::string_view target_description();

} // namespace debug
