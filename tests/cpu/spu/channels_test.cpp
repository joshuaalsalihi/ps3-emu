// SPU channels, driven by SPU programs. Expected behavior follows the Cell Broadband Engine
// Architecture (v1.01), chapter 9.

#include "support/spe_test.h"

#include <catch2/catch_test_macros.hpp>

using namespace spu;
using test::SpeTest;

// ---- Counts and invalid accesses ----

TEST_CASE_METHOD(SpeTest, "channel counts") {
    const auto result = run({"rchcnt $3, $ch21", "rchcnt $4, $ch16", "rchcnt $5, $ch5",
                             "rchcnt $6, $ch29", "rchcnt $7, $ch28", "rchcnt $8, $ch0"});
    REQUIRE(finished(result));
    CHECK(r(3) == Vector::from_words(16, 0, 0, 0)); // MFC_Cmd: free queue slots
    CHECK(preferred(4) == 1);                       // nonblocking
    CHECK(preferred(5) == 0);                       // reserved
    CHECK(preferred(6) == 0);                       // inbound mailbox: empty
    CHECK(preferred(7) == 1);                       // outbound mailbox: one free entry
    CHECK(preferred(8) == 0);                       // no events
}

TEST_CASE_METHOD(SpeTest, "reading a write channel faults") {
    const auto result = run({"rdch $3, $ch1"});
    CHECK(result.exit == SpeExit::fault);
    CHECK(result.message == "rdch from SPU_WrEventMask, not readable");
    CHECK(spe.cpu.state.pc == 0);
}

TEST_CASE_METHOD(SpeTest, "writing a reserved channel faults") {
    const auto result = run({"wrch $ch5, $3"});
    CHECK(result.exit == SpeExit::fault);
    CHECK(result.message == "wrch to channel 5, not writable");
}

// ---- Mailboxes ----

TEST_CASE_METHOD(SpeTest, "the inbound mailbox delivers values in order") {
    CHECK(spe.channels.write_in_mbox(10));
    CHECK(spe.channels.write_in_mbox(20));
    const auto result = run({"rchcnt $3, $ch29", "rdch $4, $ch29", "rdch $5, $ch29"});
    REQUIRE(finished(result));
    CHECK(preferred(3) == 2);
    CHECK(preferred(4) == 10);
    CHECK(preferred(5) == 20);
}

TEST_CASE_METHOD(SpeTest, "reading an empty inbound mailbox stalls until a value arrives") {
    auto result = run({"nop $0", "rdch $4, $ch29"});
    CHECK(stalled_on(result, channel::k_rd_in_mbox));
    CHECK(spe.cpu.state.pc == 4);

    spe.channels.write_in_mbox(7);
    result = resume();
    REQUIRE(finished(result));
    CHECK(preferred(4) == 7);
}

TEST_CASE_METHOD(SpeTest, "the inbound mailbox holds four entries") {
    for (u32 i = 0; i < 4; ++i) {
        CHECK(spe.channels.write_in_mbox(i));
    }
    CHECK_FALSE(spe.channels.write_in_mbox(4));
}

TEST_CASE_METHOD(SpeTest, "writing a full outbound mailbox stalls until it is read") {
    set_value(3, 0x55);
    set_value(4, 0x66);
    auto result = run({"wrch $ch28, $3", "wrch $ch28, $4"});
    CHECK(stalled_on(result, channel::k_wr_out_mbox));
    CHECK(spe.channels.read_out_mbox() == 0x55);

    result = resume();
    REQUIRE(finished(result));
    CHECK(spe.channels.read_out_mbox() == 0x66);
    CHECK_FALSE(spe.channels.read_out_mbox());
}

TEST_CASE_METHOD(SpeTest, "the outbound interrupt mailbox") {
    set_value(3, 0x77);
    REQUIRE(finished(run({"wrch $ch30, $3"})));
    CHECK(spe.channels.read_out_intr_mbox() == 0x77);
}

// ---- Signals ----

TEST_CASE_METHOD(SpeTest, "signal notification overwrites by default") {
    spe.channels.write_signal(0, 0x1);
    spe.channels.write_signal(0, 0x2);
    spe.channels.signal_or_mode[1] = true;
    spe.channels.write_signal(1, 0x1);
    spe.channels.write_signal(1, 0x2);
    const auto result = run({"rdch $3, $ch3", "rdch $4, $ch4", "rchcnt $5, $ch3"});
    REQUIRE(finished(result));
    CHECK(preferred(3) == 0x2);
    CHECK(preferred(4) == 0x3); // OR mode
    CHECK(preferred(5) == 0);   // reading cleared it
}

TEST_CASE_METHOD(SpeTest, "reading a signal with none pending stalls") {
    CHECK(stalled_on(run({"rdch $3, $ch4"}), channel::k_rd_sig_notify2));
}

// ---- Events ----

TEST_CASE_METHOD(SpeTest, "events are reported when enabled, until acknowledged") {
    set_value(3, event::k_in_mbox | event::k_signal1);
    spe.channels.write_in_mbox(1);
    auto result = run({"rchcnt $4, $ch0", "wrch $ch1, $3", "rchcnt $5, $ch0", "rdch $6, $ch0",
                       "rchcnt $7, $ch0", "rdch $8, $ch11"});
    REQUIRE(finished(result));
    CHECK(preferred(4) == 0); // pending, but not enabled yet
    CHECK(preferred(5) == 1);
    CHECK(preferred(6) == event::k_in_mbox);
    CHECK(preferred(7) == 0); // reading resets the count
    CHECK(preferred(8) == (event::k_in_mbox | event::k_signal1));

    // Acknowledging leaves the count at 1 if another enabled event is still pending.
    spe.channels.write_signal(0, 5);
    set_value(9, event::k_in_mbox);
    result = run({"rdch $10, $ch0", "wrch $ch2, $9", "rchcnt $11, $ch0", "rdch $12, $ch0"});
    REQUIRE(finished(result));
    CHECK(preferred(10) == (event::k_in_mbox | event::k_signal1));
    CHECK(preferred(11) == 1);
    CHECK(preferred(12) == event::k_signal1);
}

TEST_CASE_METHOD(SpeTest, "reading the event status with nothing pending stalls") {
    CHECK(stalled_on(run({"rdch $3, $ch0"}), channel::k_rd_event_stat));
}

TEST_CASE_METHOD(SpeTest, "bisled branches when an enabled event is waiting") {
    set_value(3, event::k_in_mbox);
    set_value(5, 0x18);
    spe.channels.write_in_mbox(1);
    const auto result = run({"wrch $ch1, $3", "bisled $0, $5", "il $6, 1", "stop 0x3fff", "nop $0",
                             "nop $0", "il $7, 1"}); // 0x18
    REQUIRE(finished(result));
    CHECK(preferred(6) == 0);
    CHECK(preferred(7) == 1);
}

// ---- Decrementer ----

TEST_CASE_METHOD(SpeTest, "the decrementer counts down at the timebase rate") {
    set_value(3, 1000);
    REQUIRE(finished(run({"wrch $ch7, $3"})));
    clock.advance(VirtualClock::cycle_of_timebase(clock.timebase() + 100) - clock.cycles());
    REQUIRE(finished(run({"rdch $4, $ch8"})));
    CHECK(preferred(4) == 900);
}

TEST_CASE_METHOD(SpeTest, "the decrementer event fires when the count passes zero") {
    set_value(3, event::k_decrementer);
    set_value(4, 10);
    auto result = run({"wrch $ch1, $3", "wrch $ch7, $4", "rdch $5, $ch0"});
    REQUIRE(stalled_on(result, channel::k_rd_event_stat));
    const auto deadline = spe.channels.next_deadline(channel::k_rd_event_stat);
    REQUIRE(deadline);

    clock.advance(*deadline - clock.cycles());
    result = resume();
    REQUIRE(finished(result));
    CHECK(preferred(5) == event::k_decrementer);
    // The count reads 0xffffffff once it has passed zero.
    REQUIRE(finished(run({"rdch $6, $ch8"})));
    CHECK(preferred(6) == 0xffff'ffff);
}

TEST_CASE_METHOD(SpeTest, "writing a value with bit 0 set raises the event at once") {
    set_value(3, event::k_decrementer);
    set_value(4, 0x8000'0000);
    REQUIRE(finished(run({"wrch $ch1, $3", "wrch $ch7, $4", "rchcnt $5, $ch0"})));
    CHECK(preferred(5) == 1);
}

// ---- DMA through the channels ----

namespace {

// mfc_get(lsa 0x100, ea k_main, 32 bytes, tag 5), then wait for tag 5 with an "all" update.
const std::initializer_list<std::string_view> k_get_and_wait = {
    "il $10, 0x100",   "ilhu $11, 0x2",   "il $12, 32",        "il $13, 5",       "il $14, 0x40",
    "il $15, 0",       "wrch $ch16, $10", "wrch $ch17, $15",   "wrch $ch18, $11", "wrch $ch19, $12",
    "wrch $ch20, $13", "wrch $ch21, $14", "il $16, 0x20",      "il $17, 2",       "wrch $ch22, $16",
    "wrch $ch23, $17", "rdch $3, $ch24",  "lqd $4, 0x100($0)",
};

} // namespace

TEST_CASE_METHOD(SpeTest, "waiting for a tag group stalls until its DMA completes") {
    memory.write<u32>(k_main, 0xfeed'beef);
    auto result = run(k_get_and_wait);
    REQUIRE(stalled_on(result, channel::k_rd_tag_stat));
    const auto deadline = spe.channels.next_deadline(channel::k_rd_tag_stat);
    REQUIRE(deadline);
    CHECK(local_store().read<u32>(0x100) == 0); // not yet

    clock.advance(*deadline - clock.cycles());
    result = resume();
    REQUIRE(finished(result));
    CHECK(preferred(3) == 0x20); // tag 5 complete
    CHECK(r(4).word(0) == 0xfeed'beef);
}

TEST_CASE_METHOD(SpeTest, "an immediate tag status update answers at once") {
    memory.write<u32>(k_main, 1);
    // Issue the get, then ask for the status immediately: tag 5 is still busy.
    const auto result =
        run({"il $10, 0x100", "ilhu $11, 0x2", "il $12, 32", "il $13, 5", "il $14, 0x40",
             "wrch $ch16, $10", "wrch $ch18, $11", "wrch $ch19, $12", "wrch $ch20, $13",
             "wrch $ch21, $14", "il $16, 0x21", "il $17, 0", "wrch $ch22, $16", "wrch $ch23, $17",
             "rdch $3, $ch24", "rchcnt $4, $ch21"});
    REQUIRE(finished(result));
    CHECK(preferred(3) == 0x01); // tag 0 is idle, tag 5 isn't
    CHECK(preferred(4) == 15);   // one queue slot in use
}

TEST_CASE_METHOD(SpeTest, "an \"any\" update waits for the first enabled group to finish") {
    spe.dma.instant = true;
    const auto result =
        run({"il $16, 0x6", "il $17, 1", "wrch $ch22, $16", "wrch $ch23, $17", "rdch $3, $ch24"});
    REQUIRE(finished(result)); // tags 1 and 2 have nothing outstanding
    CHECK(preferred(3) == 0x6);
}

TEST_CASE_METHOD(SpeTest, "the tag group status event") {
    spe.dma.instant = true;
    set_value(3, event::k_tag_status);
    const auto result = run({"wrch $ch1, $3", "il $16, 1", "il $17, 2", "wrch $ch22, $16",
                             "wrch $ch23, $17", "rdch $4, $ch0", "rdch $5, $ch24"});
    REQUIRE(finished(result));
    CHECK(preferred(4) == event::k_tag_status);
    CHECK(preferred(5) == 1);
}

TEST_CASE_METHOD(SpeTest, "atomic commands report through MFC_RdAtomicStat") {
    memory.write<u32>(k_main + 0x80, 41);
    // getllar, increment the word, putllc: the classic atomic increment.
    const auto result =
        run({"il $10, 0x80", "ilhu $11, 0x2", "iohl $11, 0x80", "il $14, 0xd0", "wrch $ch16, $10",
             "wrch $ch18, $11", "wrch $ch21, $14", "rdch $3, $ch27", "lqd $4, 0x80($0)",
             "ai $4, $4, 1", "stqd $4, 0x80($0)", "il $14, 0xb4", "wrch $ch21, $14",
             "rdch $5, $ch27", "rchcnt $6, $ch27"});
    REQUIRE(finished(result));
    CHECK(preferred(3) == k_atomic_getllar_done);
    CHECK(preferred(5) == 0); // success
    CHECK(preferred(6) == 0); // the status was consumed
    CHECK(memory.read<u32>(k_main + 0x80) == 42);
}

TEST_CASE_METHOD(SpeTest, "a lost reservation raises its event and fails putllc") {
    set_value(3, event::k_reservation_lost);
    auto result = run({"wrch $ch1, $3", "il $10, 0x80", "ilhu $11, 0x2", "iohl $11, 0x80",
                       "il $14, 0xd0", "wrch $ch16, $10", "wrch $ch18, $11", "wrch $ch21, $14",
                       "rdch $4, $ch27", "rdch $5, $ch0"});
    REQUIRE(stalled_on(result, channel::k_rd_event_stat));

    memory.write<u32>(k_main + 0x90, 1); // another processor writes the line
    result = resume();
    REQUIRE(finished(result));
    CHECK(preferred(5) == event::k_reservation_lost);

    REQUIRE(finished(run({"il $14, 0xb4", "wrch $ch21, $14", "rdch $6, $ch27"})));
    CHECK(preferred(6) == k_atomic_putllc_failed);
}

TEST_CASE_METHOD(SpeTest, "a full command queue stalls MFC_Cmd") {
    // 17 gets of 16 bytes with nothing completing in between.
    std::vector<std::string_view> lines = {"il $10, 0x100",  "ilhu $11, 0x2",   "il $12, 16",
                                           "il $14, 0x40",   "wrch $ch16, $10", "wrch $ch18, $11",
                                           "wrch $ch19, $12"};
    for (int i = 0; i < 17; ++i) {
        lines.push_back("wrch $ch21, $14");
    }
    lines.push_back("stop 0x3fff");
    const std::vector<u32> words = assemble(lines, 0);
    for (std::size_t i = 0; i < words.size(); ++i) {
        local_store().write<u32>(4 * static_cast<u32>(i), words[i]);
    }
    auto result = resume();
    REQUIRE(stalled_on(result, channel::k_mfc_cmd));
    CHECK(spe.dma.free_slots() == 0);

    clock.advance(*spe.channels.next_deadline(channel::k_mfc_cmd) - clock.cycles());
    result = resume();
    CHECK(finished(result));
}

TEST_CASE_METHOD(SpeTest, "DMA errors stop the SPU") {
    // A get with a 3-byte size.
    const auto result = run({"il $12, 3", "il $14, 0x40", "wrch $ch19, $12", "wrch $ch21, $14"});
    CHECK(result.exit == SpeExit::fault);
    CHECK(result.message.starts_with("bad DMA alignment"));
}

// ---- Other channels ----

TEST_CASE_METHOD(SpeTest, "SRR0, the tag mask and the machine status") {
    set_value(3, 0x1234);
    set_value(4, 0xff);
    const auto result = run(
        {"wrch $ch14, $3", "rdch $5, $ch15", "wrch $ch22, $4", "rdch $6, $ch12", "rdch $7, $ch13"});
    REQUIRE(finished(result));
    CHECK(preferred(5) == 0x1234);
    CHECK(spe.cpu.state.srr0 == 0x1234);
    CHECK(preferred(6) == 0xff);
    CHECK(preferred(7) == 0); // interrupts disabled, not isolated
}

TEST_CASE_METHOD(SpeTest, "multisource synchronization completes when earlier DMA does") {
    memory.write<u32>(k_main, 1);
    set_value(3, event::k_multisource_sync);
    auto result = run({"wrch $ch1, $3", "il $10, 0x100", "ilhu $11, 0x2", "il $12, 16",
                       "il $14, 0x40", "wrch $ch16, $10", "wrch $ch18, $11", "wrch $ch19, $12",
                       "wrch $ch21, $14", "wrch $ch9, $0", "rdch $5, $ch0"});
    REQUIRE(stalled_on(result, channel::k_rd_event_stat));
    clock.advance(*spe.channels.next_deadline(channel::k_rd_event_stat) - clock.cycles());
    REQUIRE(finished(resume()));
    CHECK(preferred(5) == event::k_multisource_sync);
}
