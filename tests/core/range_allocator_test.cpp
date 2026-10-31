#include "core/range_allocator.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("RangeAllocator hands out aligned ranges, first fit") {
    RangeAllocator ranges(0x1000, 0x10000);

    CHECK(ranges.allocate(0x100, 0x1000) == 0x1000);
    CHECK(ranges.allocate(0x100, 0x1000) == 0x2000);
    CHECK(ranges.allocate(0x100, 0x10) == 0x1100); // the first gap that fits
    CHECK(ranges.used() == 0x300);

    // A freed range is reused, if the new one fits there.
    CHECK(ranges.free(0x1000) == 0x100);
    CHECK(ranges.allocate(0x200, 0x100) == 0x1200);
    CHECK(ranges.allocate(0x100, 0x100) == 0x1000);
}

TEST_CASE("RangeAllocator reports when it is full") {
    RangeAllocator ranges(0x1000, 0x2000);
    CHECK(ranges.allocate(0x2000, 0x1000) == 0x1000);
    CHECK_FALSE(ranges.allocate(0x1, 0x1));
    CHECK_FALSE(ranges.allocate(0, 0x1));
}

TEST_CASE("RangeAllocator can claim a fixed address") {
    RangeAllocator ranges(0x1000, 0x10000);
    CHECK(ranges.allocate_at(0x4000, 0x1000));
    CHECK_FALSE(ranges.allocate_at(0x4800, 0x1000));  // overlaps the end
    CHECK_FALSE(ranges.allocate_at(0x3800, 0x1000));  // overlaps the start
    CHECK(ranges.allocate_at(0x5000, 0x1000));        // touching is fine
    CHECK_FALSE(ranges.allocate_at(0x10800, 0x1000)); // past the end
    CHECK(ranges.size_of(0x4000) == 0x1000);
    CHECK_FALSE(ranges.free(0x4800));
}
