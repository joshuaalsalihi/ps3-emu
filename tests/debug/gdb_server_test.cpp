// The GDB server, driven by packets as GDB would send them.

#include "debug/gdb_server.h"

#include "support/test_programs.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <deque>
#include <sstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using debug::GdbServer;
using namespace test;

namespace {

// Bytes GDB sends, queued up front; what the server writes is collected. Reads without waiting
// can also be made to see an interrupt, as if the user pressed Ctrl-C during a run.
class FakeConnection : public debug::Connection {
public:
    std::deque<char> input;
    std::string output;
    bool interrupt_while_running = false;

    std::optional<char> read(bool wait) override {
        if (!wait && interrupt_while_running) return '\x03';
        if (input.empty()) return std::nullopt;
        const char byte = input.front();
        input.pop_front();
        return byte;
    }
    void write(std::string_view bytes) override { output += bytes; }

    void send(std::string_view packet) {
        const std::string framed = debug::gdb::frame(packet);
        input.insert(input.end(), framed.begin(), framed.end());
    }
};

struct GdbTest {
    std::ostringstream tty;
    System system{tty};
    FakeConnection connection;
    GdbServer server{system, connection};

    explicit GdbTest(const std::vector<std::string_view>& code = k_hello_and_exit,
                     std::vector<u8> data = hello_data()) {
        system.load(make_test_program(code, std::move(data)));
    }

    std::string send(std::string_view packet) { return server.handle(packet).value_or("(none)"); }
};

// The 'g' packet's layout: r0-r31, f0-f31, then pc.
constexpr std::size_t k_pc_offset = 64 * 8 * 2;

} // namespace

TEST_CASE("GDB learns the target, its threads and why it stopped") {
    GdbTest t;
    CHECK_THAT(t.send("qSupported:multiprocess+;swbreak+"),
               ContainsSubstring("qXfer:features:read+"));
    CHECK(t.send("?") == "T05thread:1000;");
    CHECK(t.send("qfThreadInfo") == "m1000");
    CHECK(t.send("qsThreadInfo") == "l");
    CHECK(t.send("qC") == "QC1000");
    CHECK(t.send("T1000") == "OK");
    CHECK(t.send("T1234") == "E01");
    CHECK(t.send("qThreadExtraInfo,1000") == debug::gdb::to_hex(std::string_view("main, ready")));
    CHECK(t.send("vMustReplyEmpty").empty());
}

TEST_CASE("the target description arrives in pieces") {
    GdbTest t;
    std::string xml;
    std::string reply;
    do {
        reply = t.send(std::format("qXfer:features:read:target.xml:{:x},100", xml.size()));
        REQUIRE((reply[0] == 'm' || reply[0] == 'l'));
        xml += reply.substr(1);
    } while (reply[0] == 'm');
    CHECK(xml == debug::target_description());
    CHECK_THAT(xml, ContainsSubstring("<architecture>powerpc:common64</architecture>"));
    CHECK_THAT(xml, ContainsSubstring("<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\" "
                                      "regnum=\"64\"/>"));
    CHECK_THAT(xml, ContainsSubstring("<reg name=\"vrsave\" bitsize=\"32\" group=\"vector\" "
                                      "regnum=\"104\"/>"));
}

TEST_CASE("registers read and write in target byte order") {
    GdbTest t;
    // 64 eight-byte registers, pc, msr, cr (4), lr, ctr, xer (4), fpscr (4), 32 vectors, vscr
    // and vrsave (4 each).
    const std::string all = t.send("g");
    CHECK(all.size() == 2 * (64 * 8 + 8 + 8 + 4 + 8 + 8 + 4 + 4 + 32 * 16 + 4 + 4));
    CHECK(all.substr(k_pc_offset, 16) == "0000000000010000");
    CHECK(t.send("p40") == "0000000000010000"); // pc
    CHECK(t.send("p41") == "8000000000000000"); // msr: 64-bit mode

    CHECK(t.send("P3=00000000deadbeef") == "OK");
    CHECK(t.system.main_thread().cpu.state.gpr[3] == 0xdeadbeef);
    CHECK(t.send("P42=20000000") == "OK"); // cr
    CHECK(t.system.main_thread().cpu.state.cr == 0x20000000);
    CHECK(t.send("P47=000102030405060708090a0b0c0d0e0f") == "OK"); // vr0
    CHECK(t.system.main_thread().cpu.state.vr[0].word(1) == 0x04050607);
    CHECK(t.send("p47") == "000102030405060708090a0b0c0d0e0f");
    CHECK(t.send("P3=1234") == "E01"); // wrong size
    CHECK(t.send("p69") == "E01");     // no register 105

    // Writing everything back unchanged changes nothing.
    const std::string before = t.send("g");
    CHECK(t.send("G" + before) == "OK");
    CHECK(t.send("g") == before);
}

TEST_CASE("memory reads stop at the first unreadable byte; writes reach code") {
    GdbTest t;
    CHECK(t.send("m10000,4") == "38600000"); // li r3, 0
    CHECK(t.send("m0,4") == "E14");
    CHECK(t.send("M10000,4:38600005") == "OK"); // code pages are read-only to the guest
    CHECK(t.send("m10000,4") == "38600005");
    CHECK(t.send("M0,4:00000000") == "E14");
    CHECK(t.send("X10000,0:").empty()); // GDB falls back to M
}

TEST_CASE("breakpoints, stepping and continuing") {
    GdbTest t;
    CHECK(t.send("Z0,10018,4") == "OK"); // the sc that prints
    CHECK(t.send("Z2,20000,4").empty()); // watchpoints aren't supported
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.system.main_thread().cpu.state.pc == 0x10018);
    CHECK(t.tty.str().empty());

    // Stepping executes the sc, so the kernel prints.
    CHECK(t.send("vCont;s:1000;c") == "T05thread:1000;");
    CHECK(t.system.main_thread().cpu.state.pc == 0x1001c);
    CHECK(t.tty.str() == "hi\n");

    CHECK(t.send("c") == "W07");
    CHECK(t.server.last_result().exit_status == 7);
}

TEST_CASE("continuing from a breakpoint moves past it") {
    GdbTest t({"li r3, 0", "addi r3, r3, 1", "cmpwi r3, 3", "bne 0x10004", "li r11, 3", "sc"});
    CHECK(t.send("Z0,10004,4") == "OK");
    for (u64 expected = 0; expected < 3; ++expected) {
        CHECK(t.send("c") == "T05thread:1000;");
        CHECK(t.system.main_thread().cpu.state.gpr[3] == expected);
    }
    CHECK(t.send("z0,10004,4") == "OK");
    CHECK(t.send("c") == "W03");
}

TEST_CASE("faults and deadlocks are explained before the stop reply") {
    GdbTest t({"li r4, 0", "lwz r3, 0(r4)"});
    CHECK(t.send("c") == "T0bthread:1000;"); // SIGSEGV
    CHECK_THAT(t.connection.output, StartsWith("$O"));
    CHECK_THAT(t.connection.output,
               ContainsSubstring(debug::gdb::to_hex(std::string_view("invalid memory read"))));
}

TEST_CASE("Ctrl-C stops a running program") {
    GdbTest t({"b 0x10000"});
    t.connection.interrupt_while_running = true;
    CHECK(t.send("c") == "T02thread:1000;"); // SIGINT
    CHECK(t.system.instructions() == GdbServer::k_poll_interval);
}

TEST_CASE("the session acknowledges packets until told not to") {
    GdbTest t;
    t.connection.send("qAttached");
    t.connection.send("QStartNoAckMode");
    t.connection.send("qC");
    t.connection.send("k");
    CHECK(t.server.serve() == GdbServer::End::killed);
    CHECK(t.connection.output == "+" + debug::gdb::frame("1") + "+" + debug::gdb::frame("OK") +
                                     debug::gdb::frame("QC1000"));
}

TEST_CASE("detaching leaves the program to run on") {
    GdbTest t;
    t.connection.send("D");
    CHECK(t.server.serve() == GdbServer::End::detached);
    CHECK(t.system.run().exit_status == 7);
}

TEST_CASE("monitor commands show SPU threads") {
    GdbTest t(k_run_spu_thread, spu_program_data(k_spu_add_one));
    auto monitor = [&](std::string_view command) {
        const auto reply = debug::gdb::from_hex(t.send("qRcmd," + debug::gdb::to_hex(command)));
        REQUIRE(reply);
        return std::string(reply->begin(), reply->end());
    };
    CHECK_THAT(monitor("help"), ContainsSubstring("monitor spu regs N"));
    CHECK(monitor("spu") == "no SPU threads\n");

    // Stop once the SPU thread exists: at the PPU's sys_spu_thread_group_start.
    CHECK(t.send("Z0,1003c,4") == "OK");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK_THAT(monitor("spu"), StartsWith("0: thread 0x"));
    CHECK_THAT(monitor("spu"), ContainsSubstring("\"\", not started, pc 0x0\n"));
    CHECK_THAT(monitor("spu ls 0 0 8"), StartsWith("0x00000: 01 a0 0e 83 1c 00 41 83\n"));
    CHECK_THAT(monitor("spu regs 0"), StartsWith("$0    00000000 00000000 00000000 00000000\n"));
    CHECK(monitor("spu regs 9") == "no such SPU thread; see 'monitor spu'\n");
    CHECK(monitor("bogus") == "unknown command; try 'monitor help'\n");
}

// ---- Going back in time ----

namespace {

// Counts r3 up to 5, then exits with it.
const std::vector<std::string_view> k_count_to_five = {
    "li r3, 0", "addi r3, r3, 1", "cmpwi r3, 5", "bne 0x10004", "li r11, 3", "sc",
};

u64 r3(GdbTest& t) {
    return t.system.main_thread().cpu.state.gpr[3];
}
u64 pc(GdbTest& t) {
    return t.system.main_thread().cpu.state.pc;
}

} // namespace

TEST_CASE("GDB learns that it can go back") {
    GdbTest t;
    CHECK_THAT(t.send("qSupported"), ContainsSubstring("ReverseStep+;ReverseContinue+"));
}

TEST_CASE("reverse-continue goes back to the previous breakpoint hit") {
    GdbTest t(k_count_to_five);
    CHECK(t.send("Z0,10008,4") == "OK"); // cmpwi
    for (u64 count = 1; count <= 3; ++count) {
        CHECK(t.send("c") == "T05thread:1000;");
        CHECK(r3(t) == count);
    }
    const u64 at_three = t.system.instructions();

    CHECK(t.send("bc") == "T05thread:1000;");
    CHECK(r3(t) == 2);
    CHECK(pc(t) == 0x10008);
    CHECK(t.send("bc") == "T05thread:1000;");
    CHECK(r3(t) == 1);
    // No hit before that: back to the start of the recording.
    CHECK(t.send("bc") == "T05thread:1000;replaylog:begin;");
    CHECK(t.system.instructions() == 0);
    CHECK(pc(t) == 0x10000);

    // Forward again, through the same points.
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.system.instructions() == at_three);
    CHECK(t.send("z0,10008,4") == "OK");
    CHECK(t.send("c") == "W05");
}

TEST_CASE("reverse-stepi undoes one instruction of the thread") {
    GdbTest t(k_count_to_five);
    CHECK(t.send("Z0,10008,4") == "OK");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.send("c") == "T05thread:1000;"); // r3 = 2, before cmpwi
    const u64 instructions = t.system.instructions();

    CHECK(t.send("bs") == "T05thread:1000;");
    CHECK(pc(t) == 0x10004); // addi, not yet done
    CHECK(r3(t) == 1);
    CHECK(t.system.instructions() == instructions - 1);
    CHECK(t.send("bs") == "T05thread:1000;");
    CHECK(pc(t) == 0x1000c); // the previous iteration's bne
    CHECK(t.send("s") == "T05thread:1000;");
    CHECK(pc(t) == 0x10004);

    // Back to the very first instruction, then no further.
    while (t.system.main_thread().executed > 0) {
        REQUIRE(t.send("bs") == "T05thread:1000;");
    }
    CHECK(pc(t) == 0x10000);
    CHECK(t.send("bs") == "T05thread:1000;replaylog:begin;");
}

TEST_CASE("going back works across checkpoints") {
    // Checkpoints every 500 instructions, and a loop of 0x300 times 3 instructions.
    std::ostringstream tty;
    System system(tty);
    FakeConnection connection;
    GdbServer server(system, connection, {}, 500);
    system.load(make_test_program({"li r3, 0", "li r4, 0x300", "addi r3, r3, 1", "cmpw r3, r4",
                                   "bne 0x10008", "nop", "li r11, 3", "sc"},
                                  hello_data()));
    auto send = [&](std::string_view packet) { return server.handle(packet).value_or(""); };
    const lv2::PpuThread& main = system.main_thread();

    CHECK(send("Z0,10014,4") == "OK"); // the nop after the loop
    CHECK(send("c") == "T05thread:1000;");
    CHECK(main.cpu.state.gpr[3] == 0x300);
    const u64 after_loop = system.instructions();
    CHECK(server.checkpoint_count() == 5); // at 0, 500, 1000, 1500 and 2000

    CHECK(send("bs") == "T05thread:1000;");
    CHECK(system.main_thread().cpu.state.pc == 0x10010);
    CHECK(system.instructions() == after_loop - 1);

    // The only earlier hit comes before every checkpoint but the first.
    CHECK(send("Z0,10004,4") == "OK"); // li r4
    CHECK(send("bc") == "T05thread:1000;");
    CHECK(system.main_thread().cpu.state.pc == 0x10004);
    CHECK(system.instructions() == 1);
}

TEST_CASE("old checkpoints are thinned out, and going back still arrives exactly") {
    std::ostringstream tty;
    System system(tty);
    FakeConnection connection;
    GdbServer server(system, connection, {}, 10); // about 230 checkpoints' worth
    system.load(make_test_program({"li r3, 0", "li r4, 0x300", "addi r3, r3, 1", "cmpw r3, r4",
                                   "bne 0x10008", "nop", "li r11, 3", "sc"},
                                  hello_data()));
    CHECK(server.handle("Z0,10004,4") == "OK");
    CHECK(server.handle("Z0,10014,4") == "OK");
    CHECK(server.handle("c") == "T05thread:1000;"); // li r4
    CHECK(server.handle("c") == "T05thread:1000;"); // after the loop
    CHECK(server.checkpoint_count() <= GdbServer::k_max_checkpoints);

    CHECK(server.handle("bs") == "T05thread:1000;");
    CHECK(system.main_thread().cpu.state.pc == 0x10010);
    CHECK(server.handle("bc") == "T05thread:1000;");
    CHECK(system.instructions() == 1);
}

TEST_CASE("changing state from GDB stops going back past the change") {
    GdbTest t(k_count_to_five);
    CHECK(t.send("Z0,10008,4") == "OK");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.send("P3=0000000000000004") == "OK"); // skip ahead
    CHECK(t.send("bc") == "T05thread:1000;replaylog:begin;");
    CHECK(r3(t) == 4); // the change is kept
    CHECK(t.send("z0,10008,4") == "OK");
    CHECK(t.send("c") == "W05");
}

TEST_CASE("output isn't repeated when going back and forward again") {
    GdbTest t;
    CHECK(t.send("Z0,10020,4") == "OK"); // after the print
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.tty.str() == "hi\n");
    CHECK(t.send("bc") == "T05thread:1000;replaylog:begin;");
    CHECK(t.send("c") == "T05thread:1000;");
    CHECK(t.tty.str() == "hi\n");
}
