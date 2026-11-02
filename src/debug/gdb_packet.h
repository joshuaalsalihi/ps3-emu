#pragma once

// The GDB remote serial protocol's framing. A packet is "$", the payload, "#", then two hex
// digits of the payload's checksum (the sum of its bytes, modulo 256). The receiver answers
// each packet with "+" (or "-" to have it sent again), unless both sides have agreed to stop
// acknowledging. A lone 0x03 byte asks a running target to stop.
// Reference: the GDB manual, appendix "GDB Remote Serial Protocol", section "Overview".

#include "common/types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace debug::gdb {

// "$payload#xx", with '#', '$', '}' and '*' escaped as '}' followed by the byte XOR 0x20.
std::string frame(std::string_view payload);

// Assembles packets from the bytes GDB sends.
class PacketReader {
public:
    struct Event {
        enum class Kind { packet, interrupt, bad_checksum };
        Kind kind;
        std::string payload; // packet: unescaped
    };

    // Takes the next byte. Returns an event once a packet (or an interrupt) is complete.
    // Acknowledgments and other bytes between packets are ignored.
    std::optional<Event> feed(char byte);

private:
    enum class State { idle, payload, escaped, checksum_high, checksum_low };

    State state_ = State::idle;
    std::string payload_;
    u8 sum_ = 0;
    u8 checksum_ = 0;
};

// Hex encoding, two lowercase digits per byte.
std::string to_hex(std::span<const u8> bytes);
std::string to_hex(std::string_view text);
std::optional<std::vector<u8>> from_hex(std::string_view hex);
// A hex number such as "1f00", as packets write addresses and lengths.
std::optional<u64> parse_number(std::string_view hex);

} // namespace debug::gdb
