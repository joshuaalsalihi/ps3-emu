#include "hle/nid.h"

#include "common/sha1.h"

#include <array>
#include <vector>

namespace hle {

u32 nid(std::string_view function_name) {
    constexpr std::array<u8, 16> k_suffix = {0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                                             0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1a};

    std::vector<u8> input(function_name.begin(), function_name.end());
    input.insert(input.end(), k_suffix.begin(), k_suffix.end());
    const auto digest = sha1(input);
    return u32{digest[0]} | (u32{digest[1]} << 8) | (u32{digest[2]} << 16) | (u32{digest[3]} << 24);
}

} // namespace hle
