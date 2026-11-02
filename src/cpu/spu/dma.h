#pragma once

// The MFC's DMA engine: the queue of commands an SPU issues to move data between its local
// store and main memory, plus the atomic lock-line commands.
// Reference: Cell Broadband Engine Architecture (CBEA) v1.01, chapter 7 "MFC Commands".
//
// Model: the engine runs one command at a time, in the order they were issued. That satisfies
// every fence and barrier the architecture defines, since those only ever forbid reordering.
// A transfer takes time (see Dma::cost); its data moves when it completes, so code that reads
// a buffer before waiting for its tag group sees the old contents, as on hardware.

#include "common/types.h"
#include "core/clock.h"
#include "core/memory.h"
#include "core/state.h"
#include "cpu/spu/local_store.h"

#include <deque>
#include <optional>
#include <span>
#include <string>

namespace spu {

// MFC command opcodes. CBEA v1.01, Tables 7-2 to 7-5.
namespace mfc {
constexpr u32 k_put = 0x20;
constexpr u32 k_putb = 0x21;
constexpr u32 k_putf = 0x22;
constexpr u32 k_putl = 0x24;
constexpr u32 k_putlb = 0x25;
constexpr u32 k_putlf = 0x26;
constexpr u32 k_putr = 0x30;
constexpr u32 k_putrb = 0x31;
constexpr u32 k_putrf = 0x32;
constexpr u32 k_putrl = 0x34;
constexpr u32 k_putrlb = 0x35;
constexpr u32 k_putrlf = 0x36;
constexpr u32 k_get = 0x40;
constexpr u32 k_getb = 0x41;
constexpr u32 k_getf = 0x42;
constexpr u32 k_getl = 0x44;
constexpr u32 k_getlb = 0x45;
constexpr u32 k_getlf = 0x46;
constexpr u32 k_sdcrt = 0x80;
constexpr u32 k_sdcrtst = 0x81;
constexpr u32 k_sdcrz = 0x89;
constexpr u32 k_sdcrst = 0x8d;
constexpr u32 k_sdcrf = 0x8f;
constexpr u32 k_sndsig = 0xa0;
constexpr u32 k_sndsigb = 0xa1;
constexpr u32 k_sndsigf = 0xa2;
constexpr u32 k_putlluc = 0xb0;
constexpr u32 k_putllc = 0xb4;
constexpr u32 k_putqlluc = 0xb8;
constexpr u32 k_barrier = 0xc0;
constexpr u32 k_mfceieio = 0xc8;
constexpr u32 k_mfcsync = 0xcc;
constexpr u32 k_getllar = 0xd0;
} // namespace mfc

// MFC_RdAtomicStat bits. CBEA v1.01, section 9.4.
constexpr u32 k_atomic_getllar_done = 0b100;  // G
constexpr u32 k_atomic_putlluc_done = 0b010;  // U
constexpr u32 k_atomic_putllc_failed = 0b001; // S: the reservation was lost

// The parameters written to the MFC channels before a command.
struct DmaParameters {
    u32 lsa = 0;  // local store address (MFC_LSA)
    u64 ea = 0;   // effective address (MFC_EAH:MFC_EAL); for lists, EAL is the list's address
    u32 size = 0; // transfer size, or list size in bytes (MFC_Size)
    u32 tag = 0;  // tag group, 0-31 (MFC_TagID)

    void serialize(state::Archive& archive) { archive(lsa, ea, size, tag); }
};

class Dma {
public:
    static constexpr u32 k_queue_depth = 16; // CBE Programming Handbook: 16 entries per SPE
    static constexpr u32 k_lock_line_size = 128;

    // `owner` identifies the SPU's atomic reservation in `memory`.
    Dma(u32 owner, Memory& memory, LocalStore& local_store, VirtualClock& clock);

    // Debug option: every transfer completes the moment it is issued.
    bool instant = false;

    // Queues a command, or performs an immediate atomic command and returns its MFC_RdAtomicStat
    // value. The caller makes sure a queue slot is free. An invalid command sets error().
    std::optional<u32> issue(u32 opcode, const DmaParameters& parameters);

    // Performs every queued command whose completion time has come.
    void update();

    u32 free_slots() const { return k_queue_depth - static_cast<u32>(queue_.size()); }
    // One bit per tag group (bit n for group n): set if the group has nothing left to do.
    u32 completed_tags() const;
    // The clock cycle at which the next queued command completes. Nothing if the queue is empty
    // or waiting for a stalled list to be acknowledged.
    std::optional<u64> next_completion() const;
    // Commands issued to the queue and completed so far (for multisource synchronization).
    u64 issued_count() const { return issued_count_; }
    u64 completed_count() const { return completed_count_; }

    // List stall-and-notify: the tag groups (as a mask) whose list reached a flagged element
    // since the last call, and resuming one.
    u32 take_new_stalls();
    void acknowledge_stall(u32 tag);

    // True once after a reservation from getllar is lost to someone else's write.
    bool take_reservation_lost();

    // Set when a command is invalid or touches memory it can't; the SPU should stop.
    const std::optional<std::string>& error() const { return error_; }

    void serialize(state::Archive& archive) {
        archive(instant, queue_, engine_free_at_, issued_count_, completed_count_, new_stalls_,
                reserved_line_, error_);
    }

private:
    struct Command {
        u32 opcode = 0;
        DmaParameters parameters;
        u64 issued_at = 0;
        u64 cost = 0;              // cycles the engine spends on it
        std::optional<u64> finish; // when it completes, once the engine reaches it
        // List commands only.
        u32 element_count = 0;
        u32 next_element = 0; // where to resume after a stall
        u32 list_lsa = 0;     // the local store address for the next element
        bool stalled = false; // waiting for acknowledgment

        void serialize(state::Archive& archive) {
            archive(opcode, parameters, issued_at, cost, finish, element_count, next_element,
                    list_lsa, stalled);
        }
    };

    struct ListElement {
        bool stall;
        u32 size;
        u64 ea;
    };

    u64 cost(u64 bytes) const;
    ListElement list_element(const Command& command, u32 index) const;
    u64 list_cost(const Command& command, u32 first_element) const;
    bool check_transfer(u32 lsa, u64 ea, u32 size);
    bool check_memory(u64 ea, u32 size, u8 permissions);

    void execute(Command& command);
    void execute_list(Command& command);
    void transfer(bool is_get, u32 lsa, u64 ea, u32 size);
    void write_memory(u64 ea, std::span<const u8> bytes);
    void fail(std::string message);

    u32 getllar(const DmaParameters& p);
    u32 putllc(const DmaParameters& p);
    u32 putlluc(const DmaParameters& p);

    u32 owner_;
    Memory& memory_;
    LocalStore& local_store_;
    VirtualClock& clock_;

    std::deque<Command> queue_;
    u64 engine_free_at_ = 0; // when the engine finished its last command
    u64 issued_count_ = 0;
    u64 completed_count_ = 0;
    u32 new_stalls_ = 0;

    std::optional<u64> reserved_line_; // the line getllar reserved, until a put or a loss
    std::optional<std::string> error_;
};

} // namespace spu
