// Command-line entry point.

#include "loader/elf.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view k_usage = "usage: ps3emu info <file.elf>\n";

std::optional<std::vector<u8>> read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(stream), {});
}

// `ps3emu info <file>`: prints the ELF header and segment table.
int run_info(const char* path) {
    const auto data = read_file(path);
    if (!data) {
        std::cerr << "error: cannot open " << path << "\n";
        return 1;
    }

    try {
        std::cout << loader::describe_elf(loader::parse_elf(*data));
        return 0;
    } catch (const loader::LoadError& error) {
        std::cerr << "error: " << path << ": " << error.what() << "\n";
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "info") {
        return run_info(argv[2]);
    }
    std::cerr << k_usage;
    return 2;
}
