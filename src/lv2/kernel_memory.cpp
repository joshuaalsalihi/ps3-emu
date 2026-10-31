// Memory allocation (sys_memory) and the memory mapper (sys_mmapper).
//
// The mapper works in three steps: reserve a range of addresses, create a block of memory, then
// map the block somewhere inside the range. PSL1GHT's malloc uses it to grow the heap.

#include "lv2/kernel.h"

namespace lv2 {

namespace {

// Page size flags. PSL1GHT, sys/memory.h.
constexpr u64 k_page_size_1m_flag = 0x400;
constexpr u64 k_page_size_64k_flag = 0x200;

// UNVERIFIED: the granularity of mapper address ranges.
constexpr u64 k_address_range_granularity = 0x1000'0000; // 256 MB

} // namespace

u64 Kernel::page_size_from_flags(u64 flags) const {
    // UNVERIFIED: which page size applies when neither flag is set.
    if (flags & k_page_size_64k_flag) return 0x1'0000;
    if (flags & k_page_size_1m_flag) return 0x10'0000;
    return 0x10'0000;
}

// ---- sys_memory ----

s32 Kernel::allocate_memory(u64 size, u64 flags, u32& address) {
    const u64 page_size = page_size_from_flags(flags);
    if (size == 0 || size % page_size != 0) return k_error_alignment;

    const auto allocated = user_memory_.allocate(size, page_size);
    if (!allocated) return k_error_no_memory;

    memory.map(*allocated, size, Memory::k_read_write);
    address = static_cast<u32>(*allocated);
    return k_ok;
}

s32 Kernel::free_memory(u32 address) {
    const auto size = user_memory_.free(address);
    if (!size) return k_error_invalid;
    memory.unmap(address, *size);
    return k_ok;
}

u64 Kernel::user_memory_available() const {
    return k_user_memory_size - user_memory_.used();
}

// ---- sys_mmapper: address ranges ----

s32 Kernel::reserve_address_range(u64 size, u64 alignment, u32& address) {
    if (size == 0 || size % k_address_range_granularity != 0) return k_error_alignment;
    if (alignment == 0) alignment = k_address_range_granularity;

    const auto reserved = mmapper_ranges_.allocate(size, alignment);
    if (!reserved) return k_error_no_memory;

    mapped_blocks_.emplace(*reserved, RangeAllocator(*reserved, size));
    address = static_cast<u32>(*reserved);
    return k_ok;
}

s32 Kernel::release_address_range(u32 address) {
    const auto range = mapped_blocks_.find(address);
    if (range == mapped_blocks_.end()) return k_error_invalid;
    if (range->second.used() != 0) return k_error_busy;

    mapped_blocks_.erase(range);
    mmapper_ranges_.free(address);
    return k_ok;
}

// ---- sys_mmapper: memory blocks ----

s32 Kernel::create_memory_block(u64 size, u64 flags, u32& block_id) {
    const u64 page_size = page_size_from_flags(flags);
    if (size == 0 || size % page_size != 0) return k_error_alignment;

    auto block = std::make_unique<MemoryBlock>();
    block->size = size;
    block->page_size = page_size;
    block_id = add_object(std::move(block));
    return k_ok;
}

s32 Kernel::destroy_memory_block(u32 block_id) {
    MemoryBlock* block = find_object<MemoryBlock>(block_id);
    if (!block) return k_error_no_such_id;
    if (block->mapped_at != 0) return k_error_busy;
    remove_object(block_id);
    return k_ok;
}

s32 Kernel::map_memory_block(u32 address, u32 block_id) {
    MemoryBlock* block = find_object<MemoryBlock>(block_id);
    if (!block) return k_error_no_such_id;
    if (block->mapped_at != 0) return k_error_busy;
    if (address % block->page_size != 0) return k_error_alignment;

    // Find the reserved range containing the address.
    auto range = mapped_blocks_.upper_bound(address);
    if (range == mapped_blocks_.begin()) return k_error_invalid;
    --range;
    if (address >= range->second.end()) return k_error_invalid;
    if (!range->second.allocate_at(address, block->size)) return k_error_busy;

    memory.map(address, block->size, Memory::k_read_write);
    block->mapped_at = address;
    return k_ok;
}

s32 Kernel::search_and_map_memory_block(u32 range_start, u32 block_id, u32& address) {
    MemoryBlock* block = find_object<MemoryBlock>(block_id);
    if (!block) return k_error_no_such_id;
    if (block->mapped_at != 0) return k_error_busy;

    const auto range = mapped_blocks_.find(range_start);
    if (range == mapped_blocks_.end()) return k_error_invalid;

    const auto mapped = range->second.allocate(block->size, block->page_size);
    if (!mapped) return k_error_no_memory;

    memory.map(*mapped, block->size, Memory::k_read_write);
    block->mapped_at = *mapped;
    address = static_cast<u32>(*mapped);
    return k_ok;
}

s32 Kernel::unmap_memory_block(u32 address, u32& block_id) {
    for (const auto& [id, object] : objects_) {
        auto* block = dynamic_cast<MemoryBlock*>(object.get());
        if (!block || block->mapped_at != address || address == 0) continue;

        auto range = std::prev(mapped_blocks_.upper_bound(address));
        range->second.free(address);
        memory.unmap(address, block->size);
        block->mapped_at = 0;
        block_id = id;
        return k_ok;
    }
    return k_error_invalid;
}

} // namespace lv2
