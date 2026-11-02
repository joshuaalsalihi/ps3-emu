#pragma once

// Save states: the whole emulator state as bytes, and back.
//
// Each type lists its fields once, in a member function that both directions use:
//
//     void Semaphore::serialize(state::Archive& archive) {
//         archive(value, max, wake_by_priority);
//     }
//
// When saving, the archive appends each field to its bytes; when loading, it reads them back in
// the same order. Integers are stored little-endian, so a state is the same bytes on every host.
// References to other parts of the machine (memory, the clock) aren't state: the owner wires
// them up when it creates the object.

#include "common/types.h"
#include "common/vector.h"

#include <array>
#include <concepts>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace state {

// A state that can't be loaded: truncated, from another version, or inconsistent.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Archive {
public:
    // An archive that saves into bytes().
    Archive() = default;
    // An archive that loads from `input`.
    explicit Archive(std::span<const u8> input) : input_(input), loading_(true) {}

    bool loading() const { return loading_; }
    const std::vector<u8>& bytes() const { return output_; }
    std::vector<u8> take_bytes() { return std::move(output_); }
    bool at_end() const { return position_ == input_.size(); }

    template <typename... T>
    void operator()(T&... values) {
        (value(values), ...);
    }

    void value(bool& flag) {
        u8 byte = flag ? 1 : 0;
        value(byte);
        if (byte > 1) throw Error("bad boolean in save state");
        flag = byte == 1;
    }

    template <std::integral T>
    void value(T& number) {
        using U = std::make_unsigned_t<T>;
        std::array<u8, sizeof(T)> bytes{};
        if (loading_) {
            raw(bytes);
            U bits = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bits |= static_cast<U>(U{bytes[i]} << (8 * i));
            }
            number = static_cast<T>(bits);
        } else {
            const U bits = static_cast<U>(number);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bytes[i] = static_cast<u8>(bits >> (8 * i));
            }
            raw(bytes);
        }
    }

    template <typename E>
        requires std::is_enum_v<E>
    void value(E& item) {
        auto underlying = static_cast<std::underlying_type_t<E>>(item);
        value(underlying);
        item = static_cast<E>(underlying);
    }

    void value(Vector& vector) { raw(vector.bytes); }

    void value(std::string& text) {
        const u64 length = size(text.size());
        if (loading_) text.resize(length);
        raw(std::span(reinterpret_cast<u8*>(text.data()), text.size()));
    }

    template <typename T, std::size_t N>
    void value(std::array<T, N>& items) {
        for (T& item : items) {
            value(item);
        }
    }

    template <std::integral T, std::size_t N>
        requires(!std::same_as<T, bool>)
    void value(std::array<T, N>& items) {
        integers(items.data(), N);
    }

    template <typename T>
    void value(std::vector<T>& items) {
        const u64 count = size(items.size());
        if (loading_) items.assign(count, T{});
        for (T& item : items) {
            value(item);
        }
    }

    void value(std::vector<u8>& bytes) {
        const u64 count = size(bytes.size());
        if (loading_) bytes.resize(count);
        raw(bytes);
    }

    template <typename T>
    void value(std::deque<T>& items) {
        const u64 count = size(items.size());
        if (loading_) items.assign(count, T{});
        for (T& item : items) {
            value(item);
        }
    }

    template <typename T>
    void value(std::optional<T>& item) {
        bool present = item.has_value();
        value(present);
        if (loading_) {
            if (!present) {
                item.reset();
                return;
            }
            item.emplace();
        }
        if (present) value(*item);
    }

    template <typename K, typename V>
    void value(std::map<K, V>& items) {
        const u64 count = size(items.size());
        if (!loading_) {
            for (auto& [key, item] : items) {
                K copy = key;
                value(copy);
                value(item);
            }
            return;
        }
        items.clear();
        for (u64 i = 0; i < count; ++i) {
            K key{};
            value(key);
            value(items[key]);
        }
    }

    template <typename A, typename B>
    void value(std::pair<A, B>& pair) {
        value(pair.first);
        value(pair.second);
    }

    // A struct or class: its own serialize function.
    template <typename T>
        requires requires(T& object, Archive& archive) { object.serialize(archive); }
    void value(T& object) {
        object.serialize(*this);
    }

    template <typename T>
        requires requires(T& object, Archive& archive) { serialize(archive, object); }
    void value(T& object) {
        serialize(*this, object);
    }

    // Integers stored one after another, in a single pass: tables such as the RSX's registers
    // hold thousands, and a call per value makes debug builds slow.
    template <std::integral T>
    void integers(T* items, std::size_t count) {
        using U = std::make_unsigned_t<T>;
        const std::size_t size = count * sizeof(T);
        if (loading_) {
            if (size > input_.size() - position_) throw Error("save state is truncated");
            const u8* in = input_.data() + position_;
            for (std::size_t i = 0; i < count; ++i) {
                U bits = 0;
                for (std::size_t b = 0; b < sizeof(T); ++b) {
                    bits |= static_cast<U>(U{in[i * sizeof(T) + b]} << (8 * b));
                }
                items[i] = static_cast<T>(bits);
            }
            position_ += size;
            return;
        }
        const std::size_t start = output_.size();
        output_.resize(start + size);
        u8* out = output_.data() + start;
        for (std::size_t i = 0; i < count; ++i) {
            const U bits = static_cast<U>(items[i]);
            for (std::size_t b = 0; b < sizeof(T); ++b) {
                out[i * sizeof(T) + b] = static_cast<u8>(bits >> (8 * b));
            }
        }
    }

    // Bytes copied as they are.
    void raw(std::span<u8> bytes) {
        if (!loading_) {
            output_.insert(output_.end(), bytes.begin(), bytes.end());
            return;
        }
        if (bytes.size() > input_.size() - position_) throw Error("save state is truncated");
        if (!bytes.empty()) std::memcpy(bytes.data(), input_.data() + position_, bytes.size());
        position_ += bytes.size();
    }

    // The length of a sequence, stored before its elements. When loading, a length longer than
    // what is left can't be right (each element takes at least a byte), so it is rejected
    // before anything is allocated for it.
    u64 size(u64 count) {
        value(count);
        if (loading_ && count > input_.size() - position_) throw Error("save state is truncated");
        return count;
    }

    // Checks a fact the state must agree with, such as a fixed table's size.
    void expect(bool condition, const char* what) {
        if (!condition) throw Error(std::string("save state is inconsistent: ") + what);
    }

private:
    std::vector<u8> output_;
    std::span<const u8> input_;
    std::size_t position_ = 0;
    bool loading_ = false;
};

} // namespace state
