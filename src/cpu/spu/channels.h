#pragma once

// An SPU's channels: how its program talks to the MFC, the mailboxes, the signal notification
// registers, the event facility and the decrementer.
// Reference: Cell Broadband Engine Architecture (CBEA) v1.01, chapter 9.
//
// Every channel has a count. A blocking channel's count says how many accesses can complete
// right now (0 means the access stalls); a nonblocking channel's count is always 1. So an SPU
// stalls on a channel exactly when its count is 0.

#include "common/types.h"
#include "core/clock.h"
#include "core/state.h"
#include "cpu/spu/dma.h"
#include "cpu/spu/state.h"

#include <deque>
#include <optional>
#include <string_view>

namespace spu {

// Channel numbers. CBEA v1.01, Table 9-2.
namespace channel {
constexpr u32 k_rd_event_stat = 0x00;
constexpr u32 k_wr_event_mask = 0x01;
constexpr u32 k_wr_event_ack = 0x02;
constexpr u32 k_rd_sig_notify1 = 0x03;
constexpr u32 k_rd_sig_notify2 = 0x04;
constexpr u32 k_wr_dec = 0x07;
constexpr u32 k_rd_dec = 0x08;
constexpr u32 k_wr_ms_sync_req = 0x09;
constexpr u32 k_rd_event_mask = 0x0b;
constexpr u32 k_rd_tag_mask = 0x0c;
constexpr u32 k_rd_mach_stat = 0x0d;
constexpr u32 k_wr_srr0 = 0x0e;
constexpr u32 k_rd_srr0 = 0x0f;
constexpr u32 k_mfc_lsa = 0x10;
constexpr u32 k_mfc_eah = 0x11;
constexpr u32 k_mfc_eal = 0x12;
constexpr u32 k_mfc_size = 0x13;
constexpr u32 k_mfc_tag_id = 0x14;
constexpr u32 k_mfc_cmd = 0x15;
constexpr u32 k_wr_tag_mask = 0x16;
constexpr u32 k_wr_tag_update = 0x17;
constexpr u32 k_rd_tag_stat = 0x18;
constexpr u32 k_rd_list_stall_stat = 0x19;
constexpr u32 k_wr_list_stall_ack = 0x1a;
constexpr u32 k_rd_atomic_stat = 0x1b;
constexpr u32 k_wr_out_mbox = 0x1c;
constexpr u32 k_rd_in_mbox = 0x1d;
constexpr u32 k_wr_out_intr_mbox = 0x1e;
} // namespace channel

// SPU event bits (SPU_RdEventStat and friends). CBEA v1.01, section 9.11.1.
namespace event {
constexpr u32 k_multisource_sync = 0x1000;    // Ms
constexpr u32 k_privileged_attention = 0x800; // A
constexpr u32 k_reservation_lost = 0x400;     // Lr
constexpr u32 k_signal1 = 0x200;              // S1
constexpr u32 k_signal2 = 0x100;              // S2
constexpr u32 k_out_intr_mbox_free = 0x80;    // Le
constexpr u32 k_out_mbox_free = 0x40;         // Me
constexpr u32 k_decrementer = 0x20;           // Tm
constexpr u32 k_in_mbox = 0x10;               // Mb
constexpr u32 k_queue_free = 0x08;            // Qv
constexpr u32 k_list_stall = 0x02;            // Sn
constexpr u32 k_tag_status = 0x01;            // Tg
} // namespace event

// MFC_WrTagUpdate request types. CBEA v1.01, section 9.3.5.
constexpr u32 k_tag_update_immediate = 0;
constexpr u32 k_tag_update_any = 1;
constexpr u32 k_tag_update_all = 2;

// Mailbox depths on the Cell. Cell BE Programming Handbook, "Mailboxes": the inbound mailbox
// holds four entries, the outbound mailboxes one each.
constexpr std::size_t k_in_mbox_depth = 4;
constexpr std::size_t k_out_mbox_depth = 1;

// The channel's name from Table 9-2, or nothing for reserved channels.
std::optional<std::string_view> channel_name(u32 channel);

class Channels {
public:
    Channels(Dma& dma, State& state, VirtualClock& clock);

    // ---- The SPU's side ----

    bool is_readable(u32 channel) const;
    bool is_writable(u32 channel) const;
    // The channel's count (rchcnt). An access to a channel with a count of 0 stalls.
    u32 count(u32 channel);
    // rdch and wrch. The caller makes sure the channel is valid and its count is not 0.
    u32 read(u32 channel);
    void write(u32 channel, u32 value);

    // Whether bisled branches: an enabled event is waiting to be read.
    bool event_waiting() const { return event_count_ != 0; }

    // ---- The other side: the PPU and the kernel ----

    // An MMIO write to the inbound mailbox. Returns false if it is full.
    bool write_in_mbox(u32 value);
    // Reads (and removes) the outbound mailboxes' oldest entry, if any.
    std::optional<u32> read_out_mbox();
    std::optional<u32> read_out_intr_mbox();
    // A write to signal notification register 1 or 2. In OR mode the value is ORed into any
    // pending bits; otherwise it replaces them.
    void write_signal(u32 number, u32 value);
    bool signal_or_mode[2] = {false, false};

    // Brings time-dependent state up to date: DMA completions, the events they cause, the
    // decrementer and lost reservations. The access functions call this themselves.
    void update();
    // For an SPU stalled on `blocked_channel`: the next cycle at which the passing of time alone
    // might unblock it (a DMA completing, the decrementer firing), if any.
    std::optional<u64> next_deadline(u32 blocked_channel) const;

    void serialize(state::Archive& archive) {
        archive(signal_or_mode[0], signal_or_mode[1], pending_events_, event_mask_, event_count_,
                in_mbox_, out_mbox_, out_intr_mbox_, signal_[0], signal_[1], parameters_, tag_mask_,
                tag_update_request_, tag_status_, list_stall_status_, atomic_status_,
                queue_was_full_, multisource_sync_target_, decrementer_running_, decrementer_start_,
                decrementer_written_at_, decrementer_event_at_);
    }

private:
    void raise(u32 events);
    void check_tag_update();
    u32 decrementer() const;
    void start_decrementer(u32 value);
    void issue_command(u32 value);

    Dma& dma_;
    State& state_;
    VirtualClock& clock_;

    // Events: those raised and not yet acknowledged, which of them are enabled, and the
    // SPU_RdEventStat count.
    u32 pending_events_ = 0;
    u32 event_mask_ = 0;
    u32 event_count_ = 0;

    std::deque<u32> in_mbox_;
    std::deque<u32> out_mbox_;
    std::deque<u32> out_intr_mbox_;
    std::optional<u32> signal_[2];

    // MFC command parameters, and the tag-group status machinery.
    DmaParameters parameters_;
    u32 tag_mask_ = 0;
    std::optional<u32> tag_update_request_; // a conditional update waiting for its condition
    std::optional<u32> tag_status_;         // status ready for MFC_RdTagStat
    u32 list_stall_status_ = 0;
    std::optional<u32> atomic_status_;
    bool queue_was_full_ = false;
    std::optional<u64> multisource_sync_target_; // completed command count to wait for

    // The decrementer counts down at the timebase rate from `decrementer_start_`, written at
    // timebase `decrementer_written_at_`. Its event fires when bit 0 goes from 0 to 1.
    bool decrementer_running_ = false;
    u32 decrementer_start_ = 0;
    u64 decrementer_written_at_ = 0;
    u64 decrementer_event_at_ = 0; // timebase tick of the next event
};

} // namespace spu
