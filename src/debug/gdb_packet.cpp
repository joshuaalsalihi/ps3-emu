#include "debug/gdb_packet.h"

#include <charconv>
#include <format>

namespace debug::gdb {

namespace {

std::optional<u8> hex_digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
    return std::nullopt;
}

} // namespace

std::string frame(std::string_view payload) {
    std::string body;
    for (const char c : payload) {
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            body += '}';
            body += static_cast<char>(c ^ 0x20);
        } else {
            body += c;
        }
    }
    u8 sum = 0;
    for (const char c : body) {
        sum = static_cast<u8>(sum + static_cast<u8>(c));
    }
    return std::format("${}#{:02x}", body, sum);
}

std::optional<PacketReader::Event> PacketReader::feed(char byte) {
    switch (state_) {
    case State::idle:
        if (byte == '\x03') return Event{Event::Kind::interrupt, ""};
        if (byte == '$') {
            state_ = State::payload;
            payload_.clear();
            sum_ = 0;
        }
        return std::nullopt;
    case State::payload:
        if (byte == '#') {
            state_ = State::checksum_high;
            return std::nullopt;
        }
        sum_ = static_cast<u8>(sum_ + static_cast<u8>(byte));
        if (byte == '}') {
            state_ = State::escaped;
        } else {
            payload_ += byte;
        }
        return std::nullopt;
    case State::escaped:
        sum_ = static_cast<u8>(sum_ + static_cast<u8>(byte));
        payload_ += static_cast<char>(byte ^ 0x20);
        state_ = State::payload;
        return std::nullopt;
    case State::checksum_high:
        checksum_ = static_cast<u8>(hex_digit(byte).value_or(0) << 4);
        state_ = State::checksum_low;
        return std::nullopt;
    case State::checksum_low: {
        state_ = State::idle;
        const auto low = hex_digit(byte);
        if (!low || (checksum_ | *low) != sum_) return Event{Event::Kind::bad_checksum, ""};
        return Event{Event::Kind::packet, payload_};
    }
    }
    return std::nullopt;
}

std::string to_hex(std::span<const u8> bytes) {
    std::string text;
    for (const u8 byte : bytes) {
        text += std::format("{:02x}", byte);
    }
    return text;
}

std::string to_hex(std::string_view text) {
    return to_hex(std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

std::optional<std::vector<u8>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<u8> bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto high = hex_digit(hex[i]);
        const auto low = hex_digit(hex[i + 1]);
        if (!high || !low) return std::nullopt;
        bytes.push_back(static_cast<u8>(*high << 4 | *low));
    }
    return bytes;
}

std::optional<u64> parse_number(std::string_view hex) {
    u64 value = 0;
    const auto [end, error] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (hex.empty() || error != std::errc() || end != hex.data() + hex.size()) return std::nullopt;
    return value;
}

} // namespace debug::gdb
