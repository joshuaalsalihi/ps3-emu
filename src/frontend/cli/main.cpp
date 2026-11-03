// Command-line entry point.

#include "common/png.h"
#include "common/sha1.h"
#include "core/system.h"
#include "debug/gdb_server.h"
#include "debug/trace_diff.h"
#include "loader/elf.h"
#include "platform/tcp.h"

#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view k_usage = R"(usage:
  ps3emu info <file.elf>    print the ELF header and segments
  ps3emu run [options] <file.elf>
      --trace               print each instruction as it executes (to stderr)
      --max-instructions N  stop after N instructions
      --max-frames N        stop once N frames have been displayed, and exit with 0
      --instant-dma         make SPU DMA complete instantly (for debugging)
      --gpu reference|metal|null
                            render in software (the default), with Metal (macOS), or not at all
      --frames FILE         write each displayed frame's size and SHA-1 to FILE
      --dump-frames DIR     save each displayed frame as DIR/frame-0001.png, ...
      --save-state FILE     when the run stops, save the machine's state to FILE
      --gdb PORT            wait for GDB on localhost:PORT and run under its control
  ps3emu resume [options] <file.state>
                            continue a saved state, with the same options as run
  ps3emu trace-diff <a.trace> <b.trace>
                            show where two traces (from run --trace) first differ
)";

// What to do besides running: where frames and the final state go.
struct Outputs {
    std::optional<std::string> hash_file;
    std::optional<std::string> png_directory;
    std::optional<std::string> state_file;
    std::optional<u16> gdb_port;
};

// The GDB server's view of a TCP connection.
class GdbConnection : public debug::Connection {
public:
    explicit GdbConnection(TcpConnection& tcp) : tcp_(tcp) {}
    std::optional<char> read(bool wait) override { return tcp_.read(wait); }
    void write(std::string_view bytes) override { tcp_.write(bytes); }

private:
    TcpConnection& tcp_;
};

// Runs the program under GDB. Returns its result if it ended, or nothing if GDB killed it.
// If GDB detaches, the program runs on to its end.
std::optional<RunResult> run_under_gdb(System& system, const RunOptions& options, u16 port) {
    std::cerr << "waiting for GDB on localhost:" << port << "\n";
    std::string error;
    const auto tcp = TcpConnection::accept_one(port, error);
    if (!tcp) {
        std::cerr << "error: cannot listen on port " << port << ": " << error << "\n";
        return std::nullopt;
    }
    GdbConnection connection(*tcp);
    debug::GdbServer server(system, connection, options);
    switch (server.serve()) {
    case debug::GdbServer::End::program_ended: return server.last_result();
    case debug::GdbServer::End::detached: return system.run(options);
    case debug::GdbServer::End::killed: break;
    }
    std::cerr << "GDB ended the session\n";
    return std::nullopt;
}

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

bool write_file(const std::string& path, std::span<const u8> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

// `ps3emu run <file>` and `ps3emu resume <file>`: runs the program, from the start or from a
// saved state. Exits with the guest's exit status.
int run_program(std::string_view path, bool resume, RunOptions options, const Outputs& outputs) {
    const auto data = read_file(path);
    if (!data) {
        std::cerr << "error: cannot open " << path << "\n";
        return 1;
    }

    System system(std::cout);
    try {
        if (resume) {
            system.load_state(*data);
        } else {
            system.load(*data);
        }
    } catch (const std::exception& error) {
        // loader::LoadError or state::Error
        std::cerr << "error: " << path << ": " << error.what() << "\n";
        return 1;
    }

    std::ofstream hashes;
    if (outputs.hash_file) {
        hashes.open(*outputs.hash_file);
        if (!hashes) {
            std::cerr << "error: cannot write " << *outputs.hash_file << "\n";
            return 1;
        }
    }
    if (outputs.png_directory) std::filesystem::create_directories(*outputs.png_directory);
    options.on_frame = [&](const rsx::Frame& frame) {
        if (hashes.is_open()) hashes << describe_frame(frame) << "\n";
        if (!outputs.png_directory) return;
        const auto path = std::filesystem::path(*outputs.png_directory) /
                          std::format("frame-{:04}.png", frame.number);
        write_file(path.string(), encode_png(frame.width, frame.height, frame.rgb));
    };

    RunResult result;
    if (outputs.gdb_port) {
        const auto ended = run_under_gdb(system, options, *outputs.gdb_port);
        if (!ended) return 1;
        result = *ended;
    } else {
        result = system.run(options);
    }
    std::cout.flush();
    if (outputs.state_file && !write_file(*outputs.state_file, system.save_state())) {
        std::cerr << "error: cannot write " << *outputs.state_file << "\n";
        return 1;
    }
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
    case RunResult::Outcome::frame_limit:
        std::cerr << "stopped: displayed " << options.max_frames << " frames\n";
        return 0;
    case RunResult::Outcome::breakpoint: // only when debugging
    case RunResult::Outcome::stepped:
    case RunResult::Outcome::reached: return 1;
    }
    return 1;
}

std::optional<u64> parse_count(std::string_view text) {
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

// `ps3emu trace-diff <a> <b>`: exits with 0 if the traces are the same, 1 if they differ.
int run_trace_diff(std::string_view first_path, std::string_view second_path) {
    std::ifstream first{std::string(first_path)};
    std::ifstream second{std::string(second_path)};
    if (!first || !second) {
        std::cerr << "error: cannot open " << (first ? second_path : first_path) << "\n";
        return 2;
    }
    const auto difference = debug::diff_traces(first, second);
    if (!difference) {
        std::cout << "the traces are the same\n";
        return 0;
    }
    std::cout << debug::describe(*difference);
    return 1;
}

int run_command(const std::vector<std::string_view>& args, bool resume) {
    RunOptions options;
    Outputs outputs;
    std::optional<std::string_view> path;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--trace") {
            options.trace = &std::cerr;
        } else if (args[i] == "--instant-dma") {
            options.instant_dma = true;
        } else if (args[i] == "--max-frames" && i + 1 < args.size()) {
            const auto count = parse_count(args[++i]);
            if (!count) return usage_error();
            options.max_frames = *count;
        } else if (args[i] == "--max-instructions" && i + 1 < args.size()) {
            const auto count = parse_count(args[++i]);
            if (!count) return usage_error();
            options.max_instructions = *count;
        } else if (args[i] == "--gpu" && i + 1 < args.size()) {
            const std::string_view gpu = args[++i];
            if (gpu == "reference") {
                options.gpu = RunOptions::Gpu::reference;
            } else if (gpu == "metal") {
                options.gpu = RunOptions::Gpu::metal;
            } else if (gpu == "null") {
                options.gpu = RunOptions::Gpu::null;
            } else {
                return usage_error();
            }
        } else if (args[i] == "--frames" && i + 1 < args.size()) {
            outputs.hash_file = std::string(args[++i]);
        } else if (args[i] == "--dump-frames" && i + 1 < args.size()) {
            outputs.png_directory = std::string(args[++i]);
        } else if (args[i] == "--gdb" && i + 1 < args.size()) {
            const auto port = parse_count(args[++i]);
            if (!port || *port == 0 || *port > 0xffff) return usage_error();
            outputs.gdb_port = static_cast<u16>(*port);
        } else if (args[i] == "--save-state" && i + 1 < args.size()) {
            outputs.state_file = std::string(args[++i]);
        } else if (!path && !args[i].starts_with("--")) {
            path = args[i];
        } else {
            return usage_error();
        }
    }
    if (!path) return usage_error();
    return run_program(*path, resume, options, outputs);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error();

    const std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (args[0] == "info" && rest.size() == 1) return run_info(rest[0]);
    if (args[0] == "run") return run_command(rest, false);
    if (args[0] == "resume") return run_command(rest, true);
    if (args[0] == "trace-diff" && rest.size() == 2) return run_trace_diff(rest[0], rest[1]);
    return usage_error();
}
