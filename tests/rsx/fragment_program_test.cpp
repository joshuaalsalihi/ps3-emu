// Fragment programs. The anchor is a real program from the PS3 toolchain's cgcomp; the other tests
// build instructions field by field, following nouveau's layout.

#include "rsx/fragment_program.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <initializer_list>

using namespace rsx;
using namespace rsx::fragment;

namespace {

shader_float::Float f(float value) {
    return std::bit_cast<u32>(value);
}

Vec4 v(float x, float y, float z, float w) {
    return {f(x), f(y), f(z), f(w)};
}

constexpr u32 k_program = 0x10000;
constexpr u32 k_identity = 0b11'10'01'00 << 9; // x reads x ... w reads w (x lowest)

// A source word: register type, register, identity swizzle, negate.
u32 source(SourceType type, u32 index = 0, bool negate = false) {
    return static_cast<u32>(type) | index << 2 | k_identity | (negate ? 1u << 17 : 0);
}

// The operation word: opcode, destination, mask (x = bit 0), input, end.
u32 operation(u32 opcode, u32 destination, u32 mask, u32 input = 0, bool end = false) {
    return opcode << 24 | input << 13 | mask << 9 | destination << 1 | (end ? 1u : 0);
}

constexpr u32 k_always = 7u << 18; // execute on any condition code

class FragmentTest {
public:
    FragmentTest() { memory.map(k_program, Memory::k_page_size, Memory::k_read_write); }

    // Stores words as the RSX expects them, with their halves swapped.
    void store(std::initializer_list<u32> words) {
        for (const u32 word : words) {
            memory.write<u32>(k_program + size_, (word << 16) | (word >> 16));
            size_ += 4;
        }
    }

    Result run_program(const std::array<Vec4, k_input_count>& inputs) {
        const auto program = decode_program(memory, k_program);
        REQUIRE(program);
        return run(*program, inputs);
    }

    Memory memory;

private:
    u32 size_ = 0;
};

} // namespace

TEST_CASE_METHOD(FragmentTest, "cgcomp's MOV of the color decodes as such") {
    // rsx_triangle.fcg: MOV result.color, fragment.color. These are the words as stored.
    const u32 stored[] = {0x3e010100, 0xc8011c9d, 0xc8000001, 0xc8000001};
    for (u32 i = 0; i < 4; ++i) {
        memory.write<u32>(k_program + 4 * i, stored[i]);
    }
    const auto program = decode_program(memory, k_program);
    REQUIRE(program);
    REQUIRE(program->size() == 1);
    const Instruction& mov = (*program)[0];
    CHECK(mov.opcode == 1);
    CHECK(mov.end);
    CHECK(mov.destination == 0);
    CHECK(mov.mask == 0xf);
    CHECK(mov.input == k_input_color0);
    CHECK(mov.sources[0].type == SourceType::input);
    CHECK(mov.sources[0].swizzle == std::array<u8, 4>{0, 1, 2, 3});
    CHECK_FALSE(mov.conditional);

    std::array<Vec4, k_input_count> inputs{};
    inputs[k_input_color0] = v(0.25f, 0.5f, 0.75f, 1);
    CHECK(run(*program, inputs).color == v(0.25f, 0.5f, 0.75f, 1));
}

TEST_CASE_METHOD(FragmentTest, "a constant follows the instruction that reads it") {
    // R1 = COL0 + {1, 2, 3, 4}; R0 = R1 * R1.
    store({operation(0x03, 1, 0xf, k_input_color0), source(SourceType::input) | k_always,
           source(SourceType::constant), 0});
    store({f(1), f(2), f(3), f(4)});
    store({operation(0x02, 0, 0xf, 0, true), source(SourceType::temp, 1) | k_always,
           source(SourceType::temp, 1), 0});
    std::array<Vec4, k_input_count> inputs{};
    inputs[k_input_color0] = v(1, 1, 1, 1);
    CHECK(run_program(inputs).color == v(4, 9, 16, 25));
}

TEST_CASE_METHOD(FragmentTest, "MAD, masks, negation and saturation") {
    // R0.xy = sat(COL0 * COL0 + -COL0)
    store({operation(0x04, 0, 0b0011, k_input_color0, true) | 1u << 31,
           source(SourceType::input) | k_always, source(SourceType::input),
           source(SourceType::input, 0, true)});
    std::array<Vec4, k_input_count> inputs{};
    inputs[k_input_color0] = v(2, 0.5f, 3, 3);
    CHECK(run_program(inputs).color == v(1, 0, 0, 0)); // 4 - 2 = 2 -> 1; 0.25 - 0.5 -> 0
}

TEST_CASE_METHOD(FragmentTest, "fp16 registers are reported as unsupported") {
    store({operation(0x01, 0, 0xf, k_input_color0, true) | 1u << 7,
           source(SourceType::input) | k_always, 0, 0});
    const Result result = run_program({});
    REQUIRE(result.error);
    CHECK(*result.error == "fp16 fragment program registers are not supported yet");
}

namespace {

// Returns each lane's coordinates as its color, and remembers what it was asked.
class RecordingSampler : public Sampler {
public:
    Quad<Vec4> sample(u32 unit, const Quad<Vec4>& coordinates, Lookup lookup) const override {
        last_unit = unit;
        last_lookup = lookup;
        return coordinates;
    }
    mutable u32 last_unit = 99;
    mutable Lookup last_lookup = Lookup::level;
};

} // namespace

TEST_CASE_METHOD(FragmentTest, "cgcomp's TEX and TXP sample their unit at the texture coordinate") {
    // TEX result.color, fragment.texcoord[0], texture[0], 2D (as stored).
    const u32 tex[] = {0x9e011700, 0xc8011c9d, 0xc8000001, 0xc8003fe1};
    for (u32 i = 0; i < 4; ++i) {
        memory.write<u32>(k_program + 4 * i, tex[i]);
    }
    auto program = decode_program(memory, k_program);
    REQUIRE(program);
    CHECK(is_texture_lookup((*program)[0]));
    CHECK((*program)[0].input == k_input_texcoord0);

    Inputs inputs{};
    inputs[k_input_texcoord0] = v(0.25f, 0.5f, 0, 2);
    inputs[k_input_texcoord0 + 1] = v(1, 3, 0, 2);
    const RecordingSampler sampler;
    CHECK(run(*program, inputs, &sampler).color == v(0.25f, 0.5f, 0, 2));
    CHECK(sampler.last_unit == 0);
    CHECK(sampler.last_lookup == Lookup::plain);

    // TXP result.color, fragment.texcoord[1], texture[3], 2D: the coordinates divided by w.
    const u32 txp[] = {0xbe011806, 0xc8011c9d, 0xc8000001, 0xc8003fe1};
    for (u32 i = 0; i < 4; ++i) {
        memory.write<u32>(k_program + 4 * i, txp[i]);
    }
    program = decode_program(memory, k_program);
    REQUIRE(program);
    CHECK(run(*program, inputs, &sampler).color == v(0.5f, 1.5f, 0, 1));
    CHECK(sampler.last_unit == 3);
}

TEST_CASE_METHOD(FragmentTest, "a lookup without a sampler is an error") {
    store({operation(0x17, 0, 0xf, k_input_texcoord0, true), source(SourceType::input) | k_always,
           0, 0});
    const Result result = run_program({});
    REQUIRE(result.error);
}

TEST_CASE_METHOD(FragmentTest, "DDX and DDY difference neighboring lanes") {
    // R0.xy = DDX(TEX0); R0.zw = DDY(TEX0).zw
    store({operation(0x15, 0, 0b0011, k_input_texcoord0), source(SourceType::input) | k_always, 0,
           0});
    store({operation(0x16, 0, 0b1100, k_input_texcoord0, true),
           source(SourceType::input) | k_always, 0, 0});
    const auto program = decode_program(memory, k_program);
    REQUIRE(program);

    // The value is x + 10 y at each lane's pixel.
    Quad<Inputs> inputs{};
    for (u32 lane = 0; lane < k_quad_size; ++lane) {
        const float value = static_cast<float>(lane % 2) + 10.0f * static_cast<float>(lane / 2);
        inputs[lane][k_input_texcoord0] = v(value, value, value, value);
    }
    const QuadResult result = run(*program, inputs, nullptr);
    REQUIRE_FALSE(result.error);
    for (u32 lane = 0; lane < k_quad_size; ++lane) {
        CHECK(result.colors[lane] == v(1, 1, 10, 10));
    }
}
