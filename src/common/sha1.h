#pragma once

#include "common/types.h"

#include <array>
#include <span>

// SHA-1 digest of `data`. FIPS 180-4, section 6.1.
std::array<u8, 20> sha1(std::span<const u8> data);
