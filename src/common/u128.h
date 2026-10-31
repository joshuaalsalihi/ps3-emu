#pragma once

// 128-bit unsigned integer, used for exact intermediate results in soft_float.
// __int128 is a GCC/Clang extension (`__extension__` keeps -Wpedantic quiet). A port to MSVC
// would need a small struct instead.
__extension__ typedef unsigned __int128 u128;
