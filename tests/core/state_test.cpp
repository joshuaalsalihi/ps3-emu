// The save state archive: each field written is read back the same.

#include "core/state.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Point {
    s32 x = 0;
    s32 y = 0;

    void serialize(state::Archive& archive) { archive(x, y); }
};

enum class Color : u8 { red = 1, blue = 2 };

} // namespace

TEST_CASE("integers are stored little-endian, whatever the host") {
    state::Archive archive;
    u32 word = 0x11223344;
    s16 half = -2;
    archive(word, half);
    CHECK(archive.bytes() == std::vector<u8>{0x44, 0x33, 0x22, 0x11, 0xfe, 0xff});
}

TEST_CASE("every kind of field reads back as it was written") {
    bool flag = true;
    u64 number = 0x0123'4567'89ab'cdef;
    Color color = Color::blue;
    std::string text = "hello";
    std::array<u16, 3> table = {1, 2, 3};
    std::vector<Point> points = {{1, -1}, {2, -2}};
    std::deque<u32> queue = {7, 8};
    std::optional<u32> some = 5;
    std::optional<u32> none;
    std::map<u32, std::string> names = {{2, "two"}, {1, "one"}};
    Vector vector = Vector::from_words(1, 2, 3, 4);

    state::Archive saver;
    saver(flag, number, color, text, table, points, queue, some, none, names, vector);

    bool flag2 = false;
    u64 number2 = 0;
    Color color2 = Color::red;
    std::string text2;
    std::array<u16, 3> table2{};
    std::vector<Point> points2;
    std::deque<u32> queue2 = {1, 2, 3};
    std::optional<u32> some2;
    std::optional<u32> none2 = 9;
    std::map<u32, std::string> names2 = {{3, "three"}};
    Vector vector2;

    state::Archive loader(saver.bytes());
    loader(flag2, number2, color2, text2, table2, points2, queue2, some2, none2, names2, vector2);
    CHECK(loader.at_end());
    CHECK(flag2 == flag);
    CHECK(number2 == number);
    CHECK(color2 == color);
    CHECK(text2 == text);
    CHECK(table2 == table);
    REQUIRE(points2.size() == 2);
    CHECK(points2[1].x == 2);
    CHECK(points2[1].y == -2);
    CHECK(queue2 == queue);
    CHECK(some2 == some);
    CHECK(none2 == none);
    CHECK(names2 == names);
    CHECK(vector2.bytes == vector.bytes);
}

TEST_CASE("damaged states are rejected") {
    state::Archive saver;
    std::string text = "hello";
    saver(text);
    std::vector<u8> bytes = saver.bytes();

    SECTION("truncated") {
        bytes.pop_back();
        state::Archive loader(bytes);
        CHECK_THROWS_AS(loader(text), state::Error);
    }
    SECTION("a length longer than what is left") {
        bytes[3] = 0x10; // the length becomes 0x10000005
        state::Archive loader(bytes);
        CHECK_THROWS_AS(loader(text), state::Error);
    }
    SECTION("a boolean that is neither 0 nor 1") {
        const std::vector<u8> two = {2};
        state::Archive loader(two);
        bool flag = false;
        CHECK_THROWS_AS(loader(flag), state::Error);
    }
}
