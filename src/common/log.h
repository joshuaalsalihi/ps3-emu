#pragma once

#include <iostream>
#include <string_view>

// Minimal logging to stderr. Log output is for humans and never affects emulation.
inline void log_warning(std::string_view subsystem, std::string_view message) {
    std::cerr << "[" << subsystem << "] warning: " << message << "\n";
}
