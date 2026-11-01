// The MFC's DMA engine. Expected behavior follows the Cell Broadband Engine Architecture
// (v1.01), chapter 7. The timing (256 cycles plus 8 bytes per cycle) is our own model.

#include "support/spe_test.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace spu;
using Catch::Matchers::ContainsSubstring;
using test::SpeTest;

namespace {

constexpr u64 k_cost_32_bytes = 256 + 32 / 8;

DmaParameters transfer(u32 lsa, u64 ea, u32 size, u32 tag = 0) {
    return {lsa, ea, size, tag};
}

void fill(LocalStore& local_store, u32 address, u32 count, u8 first) {
    for (u32 i = 0; i < count; ++i) {
        local_store.write<u8>(address + i, static_cast<u8>(first + i));
    }
}

} // namespace

TEST_CASE_METHOD(SpeTest, "a get moves data only when it completes") {
    memory.write<u32>(k_main, 0x1234'5678);
    spe.dma.issue(mfc::k_get, transfer(0x100, k_main, 32, 5));
    CHECK(spe.dma.completed_tags() == ~(u32{1} << 5));
    CHECK(spe.dma.next_completion() == k_cost_32_bytes);

    clock.advance(k_cost_32_bytes - 1);
    spe.dma.update();
    CHECK(local_store().read<u32>(0x100) == 0);

    clock.advance(1);
    spe.dma.update();
    CHECK(local_store().read<u32>(0x100) == 0x1234'5678);
    CHECK(spe.dma.completed_tags() == ~u32{0});
    CHECK_FALSE(spe.dma.next_completion());
}

TEST_CASE_METHOD(SpeTest, "a put writes main memory") {
    fill(local_store(), 0x200, 16, 0x40);
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_put, transfer(0x200, k_main + 0x10, 16));
    spe.dma.update();
    CHECK(memory.read<u32>(k_main + 0x10) == 0x4041'4243);
    CHECK(memory.read<u32>(k_main + 0x1c) == 0x4c4d'4e4f);
}

TEST_CASE_METHOD(SpeTest, "commands run one at a time in the order issued") {
    memory.write<u32>(k_main, 1);
    memory.write<u32>(k_main + 0x20, 2);
    spe.dma.issue(mfc::k_get, transfer(0x100, k_main, 32, 1));
    spe.dma.issue(mfc::k_get, transfer(0x100, k_main + 0x20, 32, 2)); // same destination
    CHECK(spe.dma.free_slots() == Dma::k_queue_depth - 2);

    clock.advance(k_cost_32_bytes);
    spe.dma.update();
    CHECK(local_store().read<u32>(0x100) == 1);
    CHECK(spe.dma.completed_tags() == ~(u32{1} << 2));

    clock.advance(k_cost_32_bytes);
    spe.dma.update();
    CHECK(local_store().read<u32>(0x100) == 2); // the later command lands last
}

TEST_CASE_METHOD(SpeTest, "small transfers keep their position within the quadword") {
    memory.write<u32>(k_main + 0x24, 0xcafe'f00d);
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_get, transfer(0x104, k_main + 0x24, 4));
    spe.dma.update();
    CHECK(local_store().read<u32>(0x104) == 0xcafe'f00d);
    CHECK_FALSE(spe.dma.error());
}

TEST_CASE_METHOD(SpeTest, "bad sizes and alignments are errors") {
    SECTION("3 bytes") {
        spe.dma.issue(mfc::k_get, transfer(0x100, k_main, 3));
    }
    SECTION("different offsets") {
        spe.dma.issue(mfc::k_get, transfer(0x104, k_main + 8, 4));
    }
    SECTION("misaligned quadwords") {
        spe.dma.issue(mfc::k_get, transfer(0x108, k_main, 32));
    }
    SECTION("over 16 KB") {
        spe.dma.issue(mfc::k_get, transfer(0, k_main, 0x4010));
    }
    REQUIRE(spe.dma.error());
    CHECK_THAT(*spe.dma.error(), ContainsSubstring("alignment"));
}

TEST_CASE_METHOD(SpeTest, "unknown or proxy-only commands are errors") {
    spe.dma.issue(0x28, transfer(0, k_main, 16)); // puts: only the PPU's proxy queue has it
    REQUIRE(spe.dma.error());
    CHECK(*spe.dma.error() == "invalid MFC command 0x28");
}

TEST_CASE_METHOD(SpeTest, "transfers to unmapped memory are errors") {
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_put, transfer(0, 0x1000'0000, 16));
    spe.dma.update();
    REQUIRE(spe.dma.error());
    CHECK(*spe.dma.error() == "DMA to unmapped memory at 0x10000000");
}

TEST_CASE_METHOD(SpeTest, "DMA to other SPUs is reported as unsupported") {
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_sndsig, transfer(0x1c, 0xf015'400c, 4)); // SPU 1's signal 1
    spe.dma.update();
    REQUIRE(spe.dma.error());
    CHECK_THAT(*spe.dma.error(), ContainsSubstring("another SPU"));
}

TEST_CASE_METHOD(SpeTest, "list transfers walk their elements") {
    memory.write<u32>(k_main + 0x100, 0x1111'1111);
    memory.write<u32>(k_main + 0x204, 0x2222'2222);
    // Elements: 16 bytes from k_main + 0x100, then 4 bytes from k_main + 0x204.
    local_store().write<u32>(0x1000, 16);
    local_store().write<u32>(0x1004, static_cast<u32>(k_main + 0x100));
    local_store().write<u32>(0x1008, 4);
    local_store().write<u32>(0x100c, static_cast<u32>(k_main + 0x204));
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_getl, {0x400, 0x1000, 16, 3});
    spe.dma.update();
    CHECK(local_store().read<u32>(0x400) == 0x1111'1111);
    // The 4-byte element goes to the next quadword, at the address's offset within it.
    CHECK(local_store().read<u32>(0x414) == 0x2222'2222);
    CHECK(spe.dma.completed_tags() == ~u32{0});
}

TEST_CASE_METHOD(SpeTest, "a list stalls after an element with the stall-and-notify flag") {
    memory.write<u32>(k_main, 0xaaaa'aaaa);
    memory.write<u32>(k_main + 0x10, 0xbbbb'bbbb);
    local_store().write<u32>(0x1000, 0x8000'0010); // stall after this one
    local_store().write<u32>(0x1004, static_cast<u32>(k_main));
    local_store().write<u32>(0x1008, 16);
    local_store().write<u32>(0x100c, static_cast<u32>(k_main + 0x10));
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_getl, {0x400, 0x1000, 16, 7});
    spe.dma.update();
    CHECK(local_store().read<u32>(0x400) == 0xaaaa'aaaa);
    CHECK(local_store().read<u32>(0x410) == 0);
    CHECK(spe.dma.take_new_stalls() == u32{1} << 7);
    CHECK(spe.dma.take_new_stalls() == 0);
    CHECK(spe.dma.completed_tags() == ~(u32{1} << 7));

    spe.dma.acknowledge_stall(7);
    spe.dma.update();
    CHECK(local_store().read<u32>(0x410) == 0xbbbb'bbbb);
    CHECK(spe.dma.completed_tags() == ~u32{0});
}

TEST_CASE_METHOD(SpeTest, "sdcrz zeroes memory") {
    memory.write<u32>(k_main + 0x80, 0xffff'ffff);
    spe.dma.instant = true;
    spe.dma.issue(mfc::k_sdcrz, transfer(0, k_main + 0x80, 128));
    spe.dma.update();
    CHECK(memory.read<u32>(k_main + 0x80) == 0);
}

TEST_CASE_METHOD(SpeTest, "getllar and putllc update a lock line atomically") {
    memory.write<u32>(k_main + 0x84, 41);
    CHECK(spe.dma.issue(mfc::k_getllar, transfer(0x80, k_main + 0x84, 128)) ==
          k_atomic_getllar_done);
    CHECK(local_store().read<u32>(0x84) == 41); // the whole aligned line was copied

    local_store().write<u32>(0x84, 42);
    CHECK(spe.dma.issue(mfc::k_putllc, transfer(0x80, k_main + 0x80, 128)) == 0);
    CHECK(memory.read<u32>(k_main + 0x84) == 42);
    CHECK_FALSE(spe.dma.take_reservation_lost()); // our own store
}

TEST_CASE_METHOD(SpeTest, "putllc fails once someone else writes the line") {
    spe.dma.issue(mfc::k_getllar, transfer(0x80, k_main + 0x80, 128));
    memory.write<u32>(k_main + 0xfc, 7); // the PPU stores into the same line
    CHECK(spe.dma.take_reservation_lost());
    CHECK_FALSE(spe.dma.take_reservation_lost());

    local_store().write<u32>(0x80, 99);
    CHECK(spe.dma.issue(mfc::k_putllc, transfer(0x80, k_main + 0x80, 128)) ==
          k_atomic_putllc_failed);
    CHECK(memory.read<u32>(k_main + 0x80) == 0);
}

TEST_CASE_METHOD(SpeTest, "putllc without a reservation fails") {
    CHECK(spe.dma.issue(mfc::k_putllc, transfer(0x80, k_main, 128)) == k_atomic_putllc_failed);
}

TEST_CASE_METHOD(SpeTest, "putlluc and putqlluc store unconditionally") {
    local_store().write<u32>(0x80, 5);
    CHECK(spe.dma.issue(mfc::k_putlluc, transfer(0x80, k_main, 128)) == k_atomic_putlluc_done);
    CHECK(memory.read<u32>(k_main) == 5);

    local_store().write<u32>(0x80, 6);
    CHECK_FALSE(spe.dma.issue(mfc::k_putqlluc, transfer(0x80, k_main, 128, 4))); // queued
    CHECK(memory.read<u32>(k_main) == 5);
    clock.advance(256 + 128 / 8);
    spe.dma.update();
    CHECK(memory.read<u32>(k_main) == 6);
}
