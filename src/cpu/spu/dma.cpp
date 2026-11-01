#include "cpu/spu/dma.h"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace spu {

namespace {

// UNVERIFIED timing: a fixed start-up cost plus the Cell's peak DMA rate of 8 bytes per cycle
// per SPE (25.6 GB/s at 3.2 GHz, Cell BE Programming Handbook, "Memory Flow Controller"). The
// real latency depends on the bus, memory and other traffic.
constexpr u64 k_dma_setup_cycles = 256;
constexpr u64 k_dma_bytes_per_cycle = 8;

constexpr u32 k_max_transfer = 16 * 1024;
constexpr u32 k_list_element_size = 8;

// Effective addresses from here up are other SPUs' local stores and registers, which LV2 maps
// for SPU threads. PSL1GHT, sys/spu.h: SPU_THREAD_BASE.
constexpr u64 k_spu_thread_area = 0xf000'0000;

bool is_get(u32 opcode) {
    switch (opcode) {
    case mfc::k_get:
    case mfc::k_getb:
    case mfc::k_getf:
    case mfc::k_getl:
    case mfc::k_getlb:
    case mfc::k_getlf: return true;
    default: return false;
    }
}

bool is_put(u32 opcode) {
    switch (opcode) {
    case mfc::k_put:
    case mfc::k_putb:
    case mfc::k_putf:
    case mfc::k_putl:
    case mfc::k_putlb:
    case mfc::k_putlf:
    case mfc::k_putr:
    case mfc::k_putrb:
    case mfc::k_putrf:
    case mfc::k_putrl:
    case mfc::k_putrlb:
    case mfc::k_putrlf: return true;
    default: return false;
    }
}

// The "l" modifier is bit 0x04 of the transfer opcodes.
bool is_list(u32 opcode) {
    return (is_get(opcode) || is_put(opcode)) && (opcode & 0x04) != 0;
}

// Ordering commands and cache hints, which an in-order engine without caches performs instantly.
bool has_no_effect(u32 opcode) {
    switch (opcode) {
    case mfc::k_barrier:
    case mfc::k_mfceieio:
    case mfc::k_mfcsync:
    case mfc::k_sdcrt:
    case mfc::k_sdcrtst:
    case mfc::k_sdcrst:
    case mfc::k_sdcrf: return true;
    default: return false;
    }
}

} // namespace

Dma::Dma(u32 owner, Memory& memory, LocalStore& local_store, VirtualClock& clock)
    : owner_(owner), memory_(memory), local_store_(local_store), clock_(clock) {}

// ---- Issuing ----

std::optional<u32> Dma::issue(u32 opcode, const DmaParameters& p) {
    if (error_) return std::nullopt;

    // The immediate atomic commands bypass the queue. CBEA v1.01, section 7.8.
    if (opcode == mfc::k_getllar) return getllar(p);
    if (opcode == mfc::k_putllc) return putllc(p);
    if (opcode == mfc::k_putlluc) return putlluc(p);

    Command command;
    command.opcode = opcode;
    command.parameters = p;
    command.issued_at = clock_.cycles();
    command.parameters.tag = p.tag & 0x1f;

    if (is_list(opcode)) {
        // MFC_EAL holds the list's local store address; each element supplies the low word of
        // its effective address. Section 7.4.
        const u32 list_address = static_cast<u32>(p.ea);
        if (p.size % k_list_element_size != 0 || p.size > k_max_transfer ||
            list_address % k_list_element_size != 0) {
            fail(std::format("bad MFC list: size {:#x} at {:#x}", p.size, list_address));
            return std::nullopt;
        }
        command.element_count = p.size / k_list_element_size;
        command.list_lsa = p.lsa;
        command.cost = list_cost(command, 0);
    } else if (is_get(opcode) || is_put(opcode)) {
        if (!check_transfer(p.lsa, p.ea, p.size)) return std::nullopt;
        command.cost = cost(p.size);
    } else if (opcode == mfc::k_sndsig || opcode == mfc::k_sndsigb || opcode == mfc::k_sndsigf) {
        // A signal is a 4-byte put to the target's signal notification register. Section 7.9.4.
        if (p.size != 4 || !check_transfer(p.lsa, p.ea, p.size)) return std::nullopt;
        command.cost = cost(p.size);
    } else if (opcode == mfc::k_putqlluc) {
        command.cost = cost(k_lock_line_size);
    } else if (opcode == mfc::k_sdcrz) {
        command.cost = cost(p.size);
    } else if (has_no_effect(opcode)) {
        command.cost = 0;
    } else {
        fail(std::format("invalid MFC command {:#x}", opcode));
        return std::nullopt;
    }

    if (instant) command.cost = 0;
    queue_.push_back(command);
    ++issued_count_;
    return std::nullopt;
}

u64 Dma::cost(u64 bytes) const {
    if (instant) return 0;
    return k_dma_setup_cycles + (bytes + k_dma_bytes_per_cycle - 1) / k_dma_bytes_per_cycle;
}

// Transfers must be 1, 2, 4 or 8 bytes, naturally aligned and with the same position within a
// quadword in both places; or a multiple of 16 bytes, up to 16 KB, quadword-aligned in both.
// CBEA v1.01, section 9.1.4.
bool Dma::check_transfer(u32 lsa, u64 ea, u32 size) {
    const bool small = size == 1 || size == 2 || size == 4 || size == 8;
    const bool ok =
        small ? (ea % size == 0 && (lsa & 0xf) == (ea & 0xf))
              : (size % 16 == 0 && size <= k_max_transfer && lsa % 16 == 0 && ea % 16 == 0);
    if (!ok && size != 0) {
        fail(std::format("bad DMA alignment: {:#x} bytes between local store {:#x} and {:#x}", size,
                         lsa, ea));
        return false;
    }
    return true;
}

// ---- Running the queue ----

void Dma::update() {
    const u64 now = clock_.cycles();
    while (!queue_.empty() && !error_) {
        Command& head = queue_.front();
        if (head.stalled) return;
        if (!head.finish) head.finish = std::max(head.issued_at, engine_free_at_) + head.cost;
        if (*head.finish > now) return;

        execute(head);
        engine_free_at_ = *head.finish;
        if (head.stalled) return;
        queue_.pop_front();
        ++completed_count_;
    }
}

void Dma::execute(Command& command) {
    const DmaParameters& p = command.parameters;
    if (is_list(command.opcode)) return execute_list(command);
    if (is_get(command.opcode)) return transfer(true, p.lsa, p.ea, p.size);
    if (is_put(command.opcode) || command.opcode == mfc::k_sndsig ||
        command.opcode == mfc::k_sndsigb || command.opcode == mfc::k_sndsigf) {
        return transfer(false, p.lsa, p.ea, p.size);
    }
    if (command.opcode == mfc::k_putqlluc) {
        const u64 line = p.ea & ~u64{k_lock_line_size - 1};
        return transfer(false, p.lsa & ~(k_lock_line_size - 1), line, k_lock_line_size);
    }
    if (command.opcode == mfc::k_sdcrz && check_memory(p.ea, p.size, Memory::k_write)) {
        const std::vector<u8> zeros(p.size);
        write_memory(p.ea, zeros);
    }
}

Dma::ListElement Dma::list_element(const Command& command, u32 index) const {
    // Each element: {u32 stall bit and transfer size, u32 low word of the address}, with the
    // high word taken from the list command. Section 7.4.
    const u32 address = static_cast<u32>(command.parameters.ea) + index * k_list_element_size;
    const u32 size_word = local_store_.read<u32>(address);
    const u32 ea_low = local_store_.read<u32>(address + 4);
    const u64 ea_high = command.parameters.ea & 0xffff'ffff'0000'0000;
    return {(size_word >> 31) != 0, size_word & 0x7fff, ea_high | ea_low};
}

u64 Dma::list_cost(const Command& command, u32 first_element) const {
    u64 bytes = 0;
    for (u32 i = first_element; i < command.element_count; ++i) {
        bytes += list_element(command, i).size;
    }
    return cost(bytes);
}

void Dma::execute_list(Command& command) {
    const bool get = is_get(command.opcode);
    while (command.next_element < command.element_count) {
        const ListElement element = list_element(command, command.next_element);
        ++command.next_element;

        // A small element keeps the effective address's position within the quadword.
        u32 lsa = command.list_lsa;
        if (element.size < 16) lsa = (lsa & ~u32{0xf}) | static_cast<u32>(element.ea & 0xf);
        if (!check_transfer(lsa, element.ea, element.size)) return;
        transfer(get, lsa, element.ea, element.size);
        command.list_lsa = (lsa + element.size + 0xf) & ~u32{0xf};

        // A stall on the last element is ignored. Section 7.4.
        const bool last = command.next_element == command.element_count;
        if (element.stall && !last) {
            command.stalled = true;
            new_stalls_ |= u32{1} << command.parameters.tag;
            return;
        }
    }
}

bool Dma::check_memory(u64 ea, u32 size, u8 permissions) {
    if (ea >= k_spu_thread_area && ea < Memory::k_size) {
        fail(std::format("DMA to another SPU ({:#x}) is not supported yet", ea));
        return false;
    }
    if (ea + size > Memory::k_size || !memory_.is_accessible(ea, size, permissions)) {
        fail(std::format("DMA {} unmapped memory at {:#x}",
                         permissions == Memory::k_read ? "from" : "to", ea));
        return false;
    }
    return true;
}

void Dma::transfer(bool is_get, u32 lsa, u64 ea, u32 size) {
    if (size == 0 || !check_memory(ea, size, is_get ? Memory::k_read : Memory::k_write)) return;

    std::vector<u8> buffer(size);
    std::span<u8> local = local_store_.bytes();
    if (is_get) {
        memory_.read_bytes(ea, buffer);
        for (u32 i = 0; i < size; ++i) {
            local[(lsa + i) & k_lslr] = buffer[i];
        }
    } else {
        for (u32 i = 0; i < size; ++i) {
            buffer[i] = local[(lsa + i) & k_lslr];
        }
        write_memory(ea, buffer);
    }
}

// Writes to main memory. Our own write to the line we reserved ends the reservation, but that
// is not a "reservation lost" event, which is only for other processors' writes.
void Dma::write_memory(u64 ea, std::span<const u8> bytes) {
    memory_.write_bytes(ea, bytes);
    if (reserved_line_ && *reserved_line_ + k_lock_line_size > ea &&
        *reserved_line_ < ea + bytes.size()) {
        reserved_line_.reset();
    }
}

void Dma::fail(std::string message) {
    if (!error_) error_ = std::move(message);
}

// ---- Status ----

u32 Dma::completed_tags() const {
    u32 busy = 0;
    for (const Command& command : queue_) {
        busy |= u32{1} << command.parameters.tag;
    }
    return ~busy;
}

std::optional<u64> Dma::next_completion() const {
    if (queue_.empty()) return std::nullopt;
    const Command& head = queue_.front();
    if (head.stalled) return std::nullopt;
    return head.finish.value_or(std::max(head.issued_at, engine_free_at_) + head.cost);
}

u32 Dma::take_new_stalls() {
    return std::exchange(new_stalls_, 0);
}

// Only the head of the queue can be stalled, since commands run one at a time.
void Dma::acknowledge_stall(u32 tag) {
    if (queue_.empty() || !queue_.front().stalled) return; // a no-op, as section 9.3.8 suggests
    Command& head = queue_.front();
    if (head.parameters.tag != (tag & 0x1f)) return;
    head.stalled = false;
    head.finish = clock_.cycles() + list_cost(head, head.next_element);
}

bool Dma::take_reservation_lost() {
    if (!reserved_line_ || memory_.has_reservation(owner_, *reserved_line_)) return false;
    reserved_line_.reset();
    return true;
}

// ---- Atomic lock-line commands ----
// They work on a 128-byte line, the same reservation granule as the PPU's lwarx and stwcx.
// (Memory::k_line_size). CBEA v1.01, section 7.8.

u32 Dma::getllar(const DmaParameters& p) {
    const u64 line = p.ea & ~u64{k_lock_line_size - 1};
    if (!check_memory(line, k_lock_line_size, Memory::k_read)) return 0;
    transfer(true, p.lsa & ~(k_lock_line_size - 1), line, k_lock_line_size);
    memory_.reserve(owner_, line);
    reserved_line_ = line;
    return k_atomic_getllar_done;
}

u32 Dma::putllc(const DmaParameters& p) {
    const u64 line = p.ea & ~u64{k_lock_line_size - 1};
    const bool reserved = reserved_line_ == line && memory_.has_reservation(owner_, line);
    memory_.clear_reservation(owner_);
    reserved_line_.reset();
    if (!reserved) return k_atomic_putllc_failed;
    transfer(false, p.lsa & ~(k_lock_line_size - 1), line, k_lock_line_size);
    return 0;
}

u32 Dma::putlluc(const DmaParameters& p) {
    const u64 line = p.ea & ~u64{k_lock_line_size - 1};
    transfer(false, p.lsa & ~(k_lock_line_size - 1), line, k_lock_line_size);
    return k_atomic_putlluc_done;
}

} // namespace spu
