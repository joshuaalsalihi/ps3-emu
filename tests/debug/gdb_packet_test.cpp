#include "debug/gdb_packet.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace debug::gdb;

namespace {

std::vector<PacketReader::Event> feed_all(PacketReader& reader, std::string_view bytes) {
    std::vector<PacketReader::Event> events;
    for (const char byte : bytes) {
        if (auto event = reader.feed(byte)) events.push_back(*event);
    }
    return events;
}

} // namespace

TEST_CASE("packets carry the sum of their bytes") {
    CHECK(frame("OK") == "$OK#9a"); // 0x4f + 0x4b
    CHECK(frame("") == "$#00");
}

TEST_CASE("bytes that would end a packet are escaped") {
    // 'a' 0x61 + '}' 0x7d + ('#' ^ 0x20) 0x03 + 'b' 0x62 = 0x143
    CHECK(frame("a#b") == std::string("$a}\x03") + "b#43");
}

TEST_CASE("the reader assembles packets and ignores acknowledgments") {
    PacketReader reader;
    const auto events = feed_all(reader, "+$OK#9a+" + frame("a#b") + "-");
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == PacketReader::Event::Kind::packet);
    CHECK(events[0].payload == "OK");
    CHECK(events[1].payload == "a#b");
}

TEST_CASE("the reader reports bad checksums and interrupts") {
    PacketReader reader;
    const auto events = feed_all(reader, "$OK#9b\x03");
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == PacketReader::Event::Kind::bad_checksum);
    CHECK(events[1].kind == PacketReader::Event::Kind::interrupt);
}

TEST_CASE("hex helpers") {
    const std::vector<u8> bytes = {0x00, 0xab, 0x7f};
    CHECK(to_hex(bytes) == "00ab7f");
    CHECK(to_hex(std::string_view("hi")) == "6869");
    CHECK(from_hex("00AB7f") == bytes);
    CHECK_FALSE(from_hex("abc"));
    CHECK_FALSE(from_hex("zz"));
    CHECK(parse_number("1f00") == 0x1f00);
    CHECK_FALSE(parse_number(""));
    CHECK_FALSE(parse_number("12x"));
}
