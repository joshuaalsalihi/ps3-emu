#include "core/range_allocator.h"

#include <iterator>

namespace {

u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

std::optional<u64> RangeAllocator::allocate(u64 size, u64 alignment) {
    if (size == 0) return std::nullopt;

    // Try the gap before each used range, then the space after the last one.
    u64 candidate = align_up(base_, alignment);
    for (const auto& [start, length] : ranges_) {
        if (candidate + size <= start) break;
        candidate = align_up(start + length, alignment);
    }
    if (candidate + size > end_ || candidate + size < candidate) return std::nullopt;

    ranges_[candidate] = size;
    return candidate;
}

bool RangeAllocator::allocate_at(u64 address, u64 size) {
    if (size == 0 || address < base_ || address > end_ || size > end_ - address) return false;

    // The range must end before the next used range starts, and start after the previous ends.
    const auto next = ranges_.lower_bound(address);
    if (next != ranges_.end() && next->first < address + size) return false;
    if (next != ranges_.begin()) {
        const auto previous = std::prev(next);
        if (previous->first + previous->second > address) return false;
    }
    ranges_[address] = size;
    return true;
}

std::optional<u64> RangeAllocator::free(u64 address) {
    const auto it = ranges_.find(address);
    if (it == ranges_.end()) return std::nullopt;
    const u64 size = it->second;
    ranges_.erase(it);
    return size;
}

std::optional<u64> RangeAllocator::size_of(u64 address) const {
    const auto it = ranges_.find(address);
    if (it == ranges_.end()) return std::nullopt;
    return it->second;
}

u64 RangeAllocator::used() const {
    u64 total = 0;
    for (const auto& [start, length] : ranges_) {
        total += length;
    }
    return total;
}
