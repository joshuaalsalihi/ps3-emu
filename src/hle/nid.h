#pragma once

#include "common/types.h"

#include <string_view>

namespace hle {

// A library function's ID (NID): the first four bytes of SHA-1(name + a fixed 16-byte suffix),
// read as a little-endian number. Checked against the NIDs in PSL1GHT's import tables.
u32 nid(std::string_view function_name);

} // namespace hle
