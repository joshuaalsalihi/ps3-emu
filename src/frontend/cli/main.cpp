// Command-line entry point.

#include "common/png.h"
#include "common/sha1.h"
#include "core/system.h"
#include "loader/elf.h"

#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view k_usage = R"(usage:
  ps3emu info <file.elf>    print the ELF header and segments
  ps3emu run [options] <file.elf>
      --trace               print each instruction as it executes (to stderr)
      --max-instructions N  stop after N instructions
      --instant-dma         make SPU DMA complete instantly (for debugging)
      --gpu reference|null  render in software (the default), or not at all
      --frames FILE         write each displayed frame's size and SHA-1 to FILE
      --dump-frames DIR     save each displayed frame as DIR/frame-0001.png, ...
)";

// What to do with the frames the program displays.
struct FrameOutput {
    std::optional<std::string> hash_file;
    std::optional<std::string> png_directory;
};

std::optional<std::vector<u8>> read_file(std::string_view path) {
    std::ifstream stream(std::string(path), std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(stream), {});
}

int usage_error() {
    std::cerr << k_usage;
    return 2;
}

// `ps3emu info <file>`: prints the ELF header and segment table.
int run_info(std::string_view path) {
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

// "frame 1: 1280x720 sha1 <hex>": the SHA-1 is of the frame's RGB bytes, top row first.
std::string describe_frame(const rsx::Frame& frame) {
    std::string text =
        std::format("frame {}: {}x{} sha1 ", frame.number, frame.width, frame.height);
    for (const u8 byte : sha1(frame.rgb)) {
        text += std::format("{:02x}", byte);
    }
    return text;
}

// `ps3emu run <file>`: runs the program. Exits with the guest's exit status.
int run_program(std::string_view path, RunOptions options, const FrameOutput& frames) {
    const auto data = read_file(path);
    if (!data) {
        std::cerr << "error: cannot open " << path << "\n";
        return 1;
    }

    System system(std::cout);
    try {
        system.load(*data);
    } catch (const loader::LoadError& error) {
        std::cerr << "error: " << path << ": " << error.what() << "\n";
        return 1;
    }

    std::ofstream hashes;
    if (frames.hash_file) {
        hashes.open(*frames.hash_file);
        if (!hashes) {
            std::cerr << "error: cannot write " << *frames.hash_file << "\n";
            return 1;
        }
    }
    if (frames.png_directory) std::filesystem::create_directories(*frames.png_directory);
    options.on_frame = [&](const rsx::Frame& frame) {
        if (hashes.is_open()) hashes << describe_frame(frame) << "\n";
        if (!frames.png_directory) return;
        const auto path = std::filesystem::path(*frames.png_directory) /
                          std::format("frame-{:04}.png", frame.number);
        const std::vector<u8> png = encode_png(frame.width, frame.height, frame.rgb);
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
    };

    const RunResult result = system.run(options);
    std::cout.flush();
    switch (result.outcome) {
    case RunResult::Outcome::exited: return result.exit_status;
    case RunResult::Outcome::faulted:
        std::cerr << "error: guest fault after " << result.instructions
                  << " instructions: " << result.message << "\n";
        return 1;
    case RunResult::Outcome::deadlocked:
        std::cerr << "error: deadlock after " << result.instructions
                  << " instructions: " << result.message << "\n";
        return 1;
    case RunResult::Outcome::instruction_limit:
        std::cerr << "stopped: reached the limit of " << result.instructions << " instructions\n";
        return 1;
    }
    return 1;
}

std::optional<u64> parse_count(std::string_view text) {
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

int run_command(const std::vector<std::string_view>& args) {
    RunOptions options;
    FrameOutput frames;
    std::optional<std::string_view> path;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--trace") {
            options.trace = &std::cerr;
        } else if (args[i] == "--instant-dma") {
            options.instant_dma = true;
        } else if (args[i] == "--max-instructions" && i + 1 < args.size()) {
            const auto count = parse_count(args[++i]);
            if (!count) return usage_error();
            options.max_instructions = *count;
        } else if (args[i] == "--gpu" && i + 1 < args.size()) {
            const std::string_view gpu = args[++i];
            if (gpu == "reference") {
                options.gpu = RunOptions::Gpu::reference;
            } else if (gpu == "null") {
                options.gpu = RunOptions::Gpu::null;
            } else {
                return usage_error();
            }
        } else if (args[i] == "--frames" && i + 1 < args.size()) {
            frames.hash_file = std::string(args[++i]);
        } else if (args[i] == "--dump-frames" && i + 1 < args.size()) {
            frames.png_directory = std::string(args[++i]);
        } else if (!path && !args[i].starts_with("--")) {
            path = args[i];
        } else {
            return usage_error();
        }
    }
    if (!path) return usage_error();
    return run_program(*path, options, frames);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error();

    const std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (args[0] == "info" && rest.size() == 1) return run_info(rest[0]);
    if (args[0] == "run") return run_command(rest);
    return usage_error();
}
