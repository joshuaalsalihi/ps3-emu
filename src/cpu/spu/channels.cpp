#include "cpu/spu/channels.h"

#include <algorithm>
#include <utility>

namespace spu {

namespace {

using namespace channel;

struct ChannelDescription {
    u32 number;
    std::string_view name;
    bool readable;
};

// CBEA v1.01, Table 9-2.
constexpr ChannelDescription k_channels[] = {
    {k_rd_event_stat, "SPU_RdEventStat", true},
    {k_wr_event_mask, "SPU_WrEventMask", false},
    {k_wr_event_ack, "SPU_WrEventAck", false},
    {k_rd_sig_notify1, "SPU_RdSigNotify1", true},
    {k_rd_sig_notify2, "SPU_RdSigNotify2", true},
    {k_wr_dec, "SPU_WrDec", false},
    {k_rd_dec, "SPU_RdDec", true},
    {k_wr_ms_sync_req, "MFC_WrMSSyncReq", false},
    {k_rd_event_mask, "SPU_RdEventMask", true},
    {k_rd_tag_mask, "MFC_RdTagMask", true},
    {k_rd_mach_stat, "SPU_RdMachStat", true},
    {k_wr_srr0, "SPU_WrSRR0", false},
    {k_rd_srr0, "SPU_RdSRR0", true},
    {k_mfc_lsa, "MFC_LSA", false},
    {k_mfc_eah, "MFC_EAH", false},
    {k_mfc_eal, "MFC_EAL", false},
    {k_mfc_size, "MFC_Size", false},
    {k_mfc_tag_id, "MFC_TagID", false},
    {k_mfc_cmd, "MFC_Cmd", false},
    {k_wr_tag_mask, "MFC_WrTagMask", false},
    {k_wr_tag_update, "MFC_WrTagUpdate", false},
    {k_rd_tag_stat, "MFC_RdTagStat", true},
    {k_rd_list_stall_stat, "MFC_RdListStallStat", true},
    {k_wr_list_stall_ack, "MFC_WrListStallAck", false},
    {k_rd_atomic_stat, "MFC_RdAtomicStat", true},
    {k_wr_out_mbox, "SPU_WrOutMbox", false},
    {k_rd_in_mbox, "SPU_RdInMbox", true},
    {k_wr_out_intr_mbox, "SPU_WrOutIntrMbox", false},
};

const ChannelDescription* describe(u32 number) {
    for (const ChannelDescription& c : k_channels) {
        if (c.number == number) return &c;
    }
    return nullptr;
}

constexpr u64 k_decrementer_period = u64{1} << 32;

} // namespace

std::optional<std::string_view> channel_name(u32 channel) {
    if (const ChannelDescription* c = describe(channel)) return c->name;
    return std::nullopt;
}

Channels::Channels(Dma& dma, State& state, VirtualClock& clock)
    : dma_(dma), state_(state), clock_(clock) {}

bool Channels::is_readable(u32 channel) const {
    const ChannelDescription* c = describe(channel);
    return c && c->readable;
}

bool Channels::is_writable(u32 channel) const {
    const ChannelDescription* c = describe(channel);
    return c && !c->readable;
}

// ---- Counts ----

u32 Channels::count(u32 channel) {
    update();
    switch (channel) {
    case k_rd_event_stat: return event_count_;
    case k_rd_sig_notify1: return signal_[0] ? 1 : 0;
    case k_rd_sig_notify2: return signal_[1] ? 1 : 0;
    case k_mfc_cmd: return dma_.free_slots();
    case k_wr_ms_sync_req: return multisource_sync_target_ ? 0 : 1;
    case k_rd_tag_stat: return tag_status_ ? 1 : 0;
    case k_rd_list_stall_stat: return list_stall_status_ != 0 ? 1 : 0;
    case k_rd_atomic_stat: return atomic_status_ ? 1 : 0;
    case k_wr_out_mbox: return static_cast<u32>(k_out_mbox_depth - out_mbox_.size());
    case k_rd_in_mbox: return static_cast<u32>(in_mbox_.size());
    case k_wr_out_intr_mbox: return static_cast<u32>(k_out_mbox_depth - out_intr_mbox_.size());
    default:
        // Nonblocking channels always count 1; reserved channels count 0 (SPU ISA, rchcnt).
        return describe(channel) ? 1 : 0;
    }
}

// ---- Reads ----

u32 Channels::read(u32 channel) {
    update();
    switch (channel) {
    case k_rd_event_stat: event_count_ = 0; return pending_events_ & event_mask_;
    case k_rd_sig_notify1: return *std::exchange(signal_[0], std::nullopt);
    case k_rd_sig_notify2: return *std::exchange(signal_[1], std::nullopt);
    case k_rd_dec: return decrementer();
    case k_rd_event_mask: return event_mask_;
    case k_rd_tag_mask: return tag_mask_;
    case k_rd_mach_stat: return state_.interrupts_enabled ? 1 : 0; // IE; never isolated
    case k_rd_srr0: return state_.srr0;
    case k_rd_tag_stat: return *std::exchange(tag_status_, std::nullopt);
    case k_rd_list_stall_stat: return std::exchange(list_stall_status_, 0);
    case k_rd_atomic_stat: return *std::exchange(atomic_status_, std::nullopt);
    case k_rd_in_mbox: {
        const u32 value = in_mbox_.front();
        in_mbox_.pop_front();
        return value;
    }
    default: return 0;
    }
}

// ---- Writes ----

void Channels::write(u32 channel, u32 value) {
    update();
    switch (channel) {
    case k_wr_event_mask:
        event_mask_ = value;
        if (pending_events_ & event_mask_) event_count_ = 1;
        break;
    case k_wr_event_ack:
        pending_events_ &= ~value;
        if (pending_events_ & event_mask_) event_count_ = 1;
        break;
    case k_wr_dec: start_decrementer(value); break;
    case k_wr_ms_sync_req: multisource_sync_target_ = dma_.issued_count(); break;
    case k_wr_srr0: state_.srr0 = value; break;
    case k_mfc_lsa: parameters_.lsa = value & k_lslr; break;
    case k_mfc_eah: parameters_.ea = (u64{value} << 32) | (parameters_.ea & 0xffff'ffff); break;
    case k_mfc_eal: parameters_.ea = (parameters_.ea & 0xffff'ffff'0000'0000) | value; break;
    case k_mfc_size: parameters_.size = value & 0x7fff; break;
    case k_mfc_tag_id: parameters_.tag = value & 0x1f; break;
    case k_mfc_cmd: issue_command(value); break;
    case k_wr_tag_mask: tag_mask_ = value; break;
    case k_wr_tag_update:
        // An immediate request answers at once and cancels a conditional one.
        tag_update_request_ = value & 0x3;
        if (*tag_update_request_ == k_tag_update_immediate) {
            tag_update_request_.reset();
            tag_status_ = dma_.completed_tags() & tag_mask_;
            raise(event::k_tag_status);
        }
        check_tag_update();
        break;
    case k_wr_list_stall_ack: dma_.acknowledge_stall(value & 0x1f); break;
    case k_wr_out_mbox: out_mbox_.push_back(value); break;
    case k_wr_out_intr_mbox: out_intr_mbox_.push_back(value); break;
    default: break;
    }
}

// The low 16 bits are the opcode; the high 16 bits are class IDs, which only tune performance.
void Channels::issue_command(u32 value) {
    const u32 opcode = value & 0xffff;
    if (const auto status = dma_.issue(opcode, parameters_)) atomic_status_ = *status;
    if (dma_.free_slots() == 0) queue_was_full_ = true;
    update();
}

// ---- The other side ----

bool Channels::write_in_mbox(u32 value) {
    if (in_mbox_.size() >= k_in_mbox_depth) return false;
    in_mbox_.push_back(value);
    raise(event::k_in_mbox);
    return true;
}

std::optional<u32> Channels::read_out_mbox() {
    if (out_mbox_.empty()) return std::nullopt;
    const u32 value = out_mbox_.front();
    out_mbox_.pop_front();
    raise(event::k_out_mbox_free);
    return value;
}

std::optional<u32> Channels::read_out_intr_mbox() {
    if (out_intr_mbox_.empty()) return std::nullopt;
    const u32 value = out_intr_mbox_.front();
    out_intr_mbox_.pop_front();
    raise(event::k_out_intr_mbox_free);
    return value;
}

void Channels::write_signal(u32 number, u32 value) {
    std::optional<u32>& signal = signal_[number & 1];
    signal = signal_or_mode[number & 1] ? signal.value_or(0) | value : value;
    raise(number == 0 ? event::k_signal1 : event::k_signal2);
}

// ---- Time and events ----

// Raising an enabled event makes SPU_RdEventStat readable. CBEA v1.01, section 9.11.
void Channels::raise(u32 events) {
    pending_events_ |= events;
    if (events & event_mask_) event_count_ = 1;
}

void Channels::update() {
    dma_.update();
    if (queue_was_full_ && dma_.free_slots() > 0) {
        queue_was_full_ = false;
        raise(event::k_queue_free);
    }
    check_tag_update();
    if (const u32 stalls = dma_.take_new_stalls()) {
        list_stall_status_ |= stalls;
        raise(event::k_list_stall);
    }
    if (dma_.take_reservation_lost()) raise(event::k_reservation_lost);
    if (multisource_sync_target_ && dma_.completed_count() >= *multisource_sync_target_) {
        multisource_sync_target_.reset();
        raise(event::k_multisource_sync);
    }
    if (decrementer_running_ && clock_.timebase() >= decrementer_event_at_) {
        decrementer_event_at_ += k_decrementer_period;
        raise(event::k_decrementer);
    }
}

// A conditional request completes when any (or all) of the enabled tag groups are done.
void Channels::check_tag_update() {
    if (!tag_update_request_) return;
    const u32 done = dma_.completed_tags() & tag_mask_;
    const bool satisfied = *tag_update_request_ == k_tag_update_any ? done != 0 : done == tag_mask_;
    if (!satisfied) return;
    tag_update_request_.reset();
    tag_status_ = done;
    raise(event::k_tag_status);
}

// Mailboxes and signals only change when someone else writes them, so they have no deadline.
std::optional<u64> Channels::next_deadline(u32 blocked_channel) const {
    switch (blocked_channel) {
    case k_mfc_cmd:
    case k_wr_ms_sync_req:
    case k_rd_tag_stat:
    case k_rd_list_stall_stat: return dma_.next_completion();
    case k_rd_event_stat: {
        std::optional<u64> deadline = dma_.next_completion();
        if (decrementer_running_ && (event_mask_ & event::k_decrementer)) {
            const u64 at = VirtualClock::cycle_of_timebase(decrementer_event_at_);
            deadline = std::min(deadline.value_or(at), at);
        }
        return deadline;
    }
    default: return std::nullopt;
    }
}

// UNVERIFIED: the decrementer reads 0 and doesn't count until first written. The CBEA says it
// starts with a write to SPU_WrDec; what LV2 does before that is unknown.
u32 Channels::decrementer() const {
    if (!decrementer_running_) return decrementer_start_;
    return decrementer_start_ - static_cast<u32>(clock_.timebase() - decrementer_written_at_);
}

// The event fires when the count passes from 0 to 0xffffffff, or at once if the write itself
// sets bit 0 when it was clear. CBEA v1.01, section 9.7.1.
void Channels::start_decrementer(u32 value) {
    const u32 old = decrementer();
    decrementer_running_ = true;
    decrementer_start_ = value;
    decrementer_written_at_ = clock_.timebase();
    decrementer_event_at_ = decrementer_written_at_ + u64{value} + 1;
    if ((old >> 31) == 0 && (value >> 31) == 1) raise(event::k_decrementer);
}

} // namespace spu
