#pragma once

// Compares two traces from `ps3emu run --trace`. Runs are deterministic, so two traces of the
// same program agree line for line until something differs: a change between two builds, or
// between two sets of options. The first difference is where to start looking.

#include "common/types.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace debug {

struct TraceDifference {
    u64 line = 0;         // the first line that differs, counting from 1
    u64 instructions = 0; // instructions both traces show before it
    std::string thread;   // the thread running there: its "--- thread ..." header, if any
    std::vector<std::string> context; // the last lines both traces share before it
    // The differing line in each trace, or nothing if that trace ended there.
    std::optional<std::string> first;
    std::optional<std::string> second;
};

// The first difference between two traces, or nothing if they are the same.
std::optional<TraceDifference> diff_traces(std::istream& first, std::istream& second,
                                           std::size_t context_lines = 5);

// A report of the difference, ending with the two lines, marked - and +.
std::string describe(const TraceDifference& difference);

} // namespace debug
