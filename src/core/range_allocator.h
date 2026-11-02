#pragma once

#include "common/types.h"
#include "core/state.h"

#include <map>
#include <optional>

// Hands out ranges of guest address space within [base, base + size), first fit. It only does
// the bookkeeping; mapping the memory is up to the caller.
class RangeAllocator {
public:
    RangeAllocator(u64 base, u64 size) : base_(base), end_(base + size) {}

    // Returns the start of a free range of `size` bytes aligned to `alignment` (a power of two).
    std::optional<u64> allocate(u64 size, u64 alignment);
    // Claims exactly [address, address + size). Returns false if any of it is taken or outside.
    bool allocate_at(u64 address, u64 size);
    // Frees the range starting at `address`. Returns its size, or nothing if no range starts there.
    std::optional<u64> free(u64 address);
    std::optional<u64> size_of(u64 address) const;

    u64 base() const { return base_; }
    u64 end() const { return end_; }
    u64 used() const;

    void serialize(state::Archive& archive) { archive(base_, end_, ranges_); }

private:
    u64 base_;
    u64 end_;
    std::map<u64, u64> ranges_; // start -> size, sorted, so allocation is deterministic
};
