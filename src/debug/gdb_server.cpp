#include "debug/gdb_server.h"

#include "common/endian.h"

#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <vector>

namespace debug {

namespace {

// Register numbers, as the target description assigns them. They follow GDB's traditional
// numbering for 64-bit PowerPC with AltiVec, so a 'g' packet lists them in this order.
constexpr u32 k_reg_r0 = 0;
constexpr u32 k_reg_f0 = 32;
constexpr u32 k_reg_pc = 64;
constexpr u32 k_reg_msr = 65;
constexpr u32 k_reg_cr = 66;
constexpr u32 k_reg_lr = 67;
constexpr u32 k_reg_ctr = 68;
constexpr u32 k_reg_xer = 69;
constexpr u32 k_reg_fpscr = 70;
constexpr u32 k_reg_vr0 = 71;
constexpr u32 k_reg_vscr = 103;
constexpr u32 k_reg_vrsave = 104;
constexpr u32 k_register_count = 105;

// The guest can't read the MSR, so GDB is shown a fixed value: 64-bit mode (SF).
constexpr u64 k_msr = u64{1} << 63;

// The longest memory read one reply carries, within the packet size we announce.
constexpr u64 k_max_memory_read = 0x800;
constexpr std::string_view k_packet_size = "1000";

u32 register_size(u32 number) {
    if (number >= k_reg_vr0 && number < k_reg_vscr) return 16;
    if (number == k_reg_cr || number == k_reg_xer || number == k_reg_fpscr ||
        number == k_reg_vscr || number == k_reg_vrsave) {
        return 4;
    }
    return 8;
}

std::string hex_be(u64 value, u32 size) {
    std::array<u8, 8> bytes{};
    store_be<u64>(bytes.data(), value);
    return gdb::to_hex(std::span(bytes).last(size));
}

u64 load_be_sized(std::span<const u8> bytes) {
    u64 value = 0;
    for (const u8 byte : bytes) {
        value = value << 8 | byte;
    }
    return value;
}

// A number in a monitor command: hex with "0x", otherwise decimal.
std::optional<u64> parse_argument(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
        base = 16;
    }
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t at = text.find(separator);
        if (!text.empty() || at != std::string_view::npos) parts.push_back(text.substr(0, at));
        if (at == std::string_view::npos) return parts;
        text.remove_prefix(at + 1);
    }
}

std::string_view status_name(lv2::ThreadStatus status) {
    switch (status) {
    case lv2::ThreadStatus::stopped: return "not started";
    case lv2::ThreadStatus::ready: return "ready";
    case lv2::ThreadStatus::waiting: return "waiting";
    case lv2::ThreadStatus::finished: return "finished";
    }
    return "?";
}

std::string build_target_description() {
    std::string xml = "<?xml version=\"1.0\"?>\n"
                      "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
                      "<target version=\"1.0\">\n"
                      "  <architecture>powerpc:common64</architecture>\n"
                      "  <feature name=\"org.gnu.gdb.power.core\">\n";
    for (u32 i = 0; i < 32; ++i) {
        xml += std::format("    <reg name=\"r{}\" bitsize=\"64\" type=\"uint64\" regnum=\"{}\"/>\n",
                           i, k_reg_r0 + i);
    }
    xml += std::format("    <reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\" regnum=\"{}\"/>\n"
                       "    <reg name=\"msr\" bitsize=\"64\" type=\"uint64\" regnum=\"{}\"/>\n"
                       "    <reg name=\"cr\" bitsize=\"32\" type=\"uint32\" regnum=\"{}\"/>\n"
                       "    <reg name=\"lr\" bitsize=\"64\" type=\"code_ptr\" regnum=\"{}\"/>\n"
                       "    <reg name=\"ctr\" bitsize=\"64\" type=\"uint64\" regnum=\"{}\"/>\n"
                       "    <reg name=\"xer\" bitsize=\"32\" type=\"uint32\" regnum=\"{}\"/>\n"
                       "  </feature>\n"
                       "  <feature name=\"org.gnu.gdb.power.fpu\">\n",
                       k_reg_pc, k_reg_msr, k_reg_cr, k_reg_lr, k_reg_ctr, k_reg_xer);
    for (u32 i = 0; i < 32; ++i) {
        xml += std::format(
            "    <reg name=\"f{}\" bitsize=\"64\" type=\"ieee_double\" regnum=\"{}\"/>\n", i,
            k_reg_f0 + i);
    }
    xml += std::format("    <reg name=\"fpscr\" bitsize=\"32\" group=\"float\" regnum=\"{}\"/>\n"
                       "  </feature>\n"
                       "  <feature name=\"org.gnu.gdb.power.altivec\">\n"
                       "    <vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/>\n"
                       "    <vector id=\"v4i32\" type=\"int32\" count=\"4\"/>\n"
                       "    <vector id=\"v8i16\" type=\"int16\" count=\"8\"/>\n"
                       "    <vector id=\"v16i8\" type=\"int8\" count=\"16\"/>\n"
                       "    <union id=\"vec128\">\n"
                       "      <field name=\"uint128\" type=\"uint128\"/>\n"
                       "      <field name=\"v4_float\" type=\"v4f\"/>\n"
                       "      <field name=\"v4_int32\" type=\"v4i32\"/>\n"
                       "      <field name=\"v8_int16\" type=\"v8i16\"/>\n"
                       "      <field name=\"v16_int8\" type=\"v16i8\"/>\n"
                       "    </union>\n",
                       k_reg_fpscr);
    for (u32 i = 0; i < 32; ++i) {
        xml +=
            std::format("    <reg name=\"vr{}\" bitsize=\"128\" type=\"vec128\" regnum=\"{}\"/>\n",
                        i, k_reg_vr0 + i);
    }
    xml += std::format("    <reg name=\"vscr\" bitsize=\"32\" group=\"vector\" regnum=\"{}\"/>\n"
                       "    <reg name=\"vrsave\" bitsize=\"32\" group=\"vector\" regnum=\"{}\"/>\n"
                       "  </feature>\n"
                       "</target>\n",
                       k_reg_vscr, k_reg_vrsave);
    return xml;
}

} // namespace

std::string_view target_description() {
    static const std::string xml = build_target_description();
    return xml;
}

GdbServer::GdbServer(System& system, Connection& connection, RunOptions options,
                     u64 checkpoint_interval)
    : system_(system), connection_(connection), options_(std::move(options)),
      checkpoint_interval_(checkpoint_interval) {}

// GDB always has a current thread: the one the last stop named, while it lives, otherwise the
// first thread that does.
void GdbServer::update_stopped_thread() {
    const lv2::PpuThread* stopped = system_.kernel().find_thread(stopped_thread_);
    if (stopped && stopped->status != lv2::ThreadStatus::finished) return;
    for (const auto& thread : system_.kernel().threads()) {
        if (thread->status != lv2::ThreadStatus::finished) {
            stopped_thread_ = thread->id;
            return;
        }
    }
}

GdbServer::End GdbServer::serve() {
    while (!end_) {
        const std::optional<char> byte = connection_.read(true);
        if (!byte) return End::killed; // the connection closed
        const auto event = reader_.feed(*byte);
        if (!event) continue;
        switch (event->kind) {
        case gdb::PacketReader::Event::Kind::interrupt: continue; // already stopped
        case gdb::PacketReader::Event::Kind::bad_checksum:
            if (acknowledging_) connection_.write("-");
            continue;
        case gdb::PacketReader::Event::Kind::packet: break;
        }
        if (acknowledging_) connection_.write("+");
        if (const auto reply = handle(event->payload)) connection_.write(gdb::frame(*reply));
    }
    return *end_;
}

std::optional<std::string> GdbServer::handle(std::string_view packet) {
    update_stopped_thread();
    if (packet.empty()) return "";
    const std::string_view rest = packet.substr(1);
    switch (packet[0]) {
    case '?': return std::format("T05thread:{:x};", stopped_thread_);
    case 'g': {
        const lv2::PpuThread* thread = selected_thread();
        return thread ? read_registers(*thread) : "E01";
    }
    case 'G': {
        lv2::PpuThread* thread = selected_thread();
        if (!thread || !write_registers(*thread, rest)) return "E01";
        forget_checkpoints();
        return "OK";
    }
    case 'p': {
        const lv2::PpuThread* thread = selected_thread();
        const auto number = gdb::parse_number(rest);
        if (!thread || !number) return "E01";
        return read_register(*thread, static_cast<u32>(*number)).value_or("E01");
    }
    case 'P': {
        lv2::PpuThread* thread = selected_thread();
        const std::size_t equals = rest.find('=');
        if (!thread || equals == std::string_view::npos) return "E01";
        const auto number = gdb::parse_number(rest.substr(0, equals));
        const auto bytes = gdb::from_hex(rest.substr(equals + 1));
        if (!number || !bytes) return "E01";
        if (!write_register(*thread, static_cast<u32>(*number), *bytes)) return "E01";
        forget_checkpoints();
        return "OK";
    }
    case 'm': {
        const auto fields = split(rest, ',');
        if (fields.size() != 2) return "E01";
        const auto address = gdb::parse_number(fields[0]);
        const auto length = gdb::parse_number(fields[1]);
        if (!address || !length) return "E01";
        return read_memory(*address, *length);
    }
    case 'M': {
        const std::size_t colon = rest.find(':');
        if (colon == std::string_view::npos) return "E01";
        const auto fields = split(rest.substr(0, colon), ',');
        const auto bytes = gdb::from_hex(rest.substr(colon + 1));
        if (fields.size() != 2 || !bytes) return "E01";
        const auto address = gdb::parse_number(fields[0]);
        if (!address) return "E01";
        if (!system_.memory().debug_write(*address, *bytes)) return "E14";
        forget_checkpoints();
        return "OK";
    }
    case 'Z':
    case 'z': {
        // Software (0) and hardware (1) breakpoints are the same to us; watchpoints aren't
        // supported, which GDB learns from the empty reply.
        const auto fields = split(rest, ',');
        if (fields.size() < 2 || (fields[0] != "0" && fields[0] != "1")) return "";
        const auto address = gdb::parse_number(fields[1]);
        if (!address) return "E01";
        if (packet[0] == 'Z') {
            breakpoints_.insert(*address);
        } else {
            breakpoints_.erase(*address);
        }
        return "OK";
    }
    case 'H': {
        // Hg picks the thread for register access; 0 and -1 mean any.
        if (rest.empty()) return "E01";
        if (rest[0] == 'g') {
            const auto id = gdb::parse_number(rest.substr(1));
            selected_thread_ = id ? static_cast<u32>(*id) : 0;
        }
        return "OK";
    }
    case 'T': {
        const auto id = gdb::parse_number(rest);
        const lv2::PpuThread* thread =
            id ? system_.kernel().find_thread(static_cast<u32>(*id)) : nullptr;
        return thread && thread->status != lv2::ThreadStatus::finished ? "OK" : "E01";
    }
    case 'c': return resume(std::nullopt);
    case 'b':
        // Reverse execution: bs steps back, bc continues back.
        if (packet == "bc") return reverse_continue();
        if (packet == "bs") {
            const lv2::PpuThread* thread = selected_thread();
            return thread ? reverse_step(thread->id) : "E01";
        }
        return "";
    case 's': {
        const lv2::PpuThread* thread = selected_thread();
        return thread ? resume(thread->id) : "E01";
    }
    case 'v':
        if (packet == "vCont?") return "vCont;c;C;s;S";
        if (packet.starts_with("vCont;")) {
            // Each action applies to one thread or to all. A step for any thread makes this a
            // step of that thread; the other threads run as the scheduler gives them turns.
            for (const std::string_view action : split(packet.substr(6), ';')) {
                if (action.empty() || (action[0] != 's' && action[0] != 'S')) continue;
                const std::size_t colon = action.find(':');
                std::optional<u64> id;
                if (colon != std::string_view::npos)
                    id = gdb::parse_number(action.substr(colon + 1));
                if (id && *id != 0 && *id != static_cast<u64>(-1)) {
                    return resume(static_cast<u32>(*id));
                }
                const lv2::PpuThread* thread = selected_thread();
                return thread ? resume(thread->id) : "E01";
            }
            return resume(std::nullopt);
        }
        return "";
    case 'q':
    case 'Q': return query(packet);
    case 'k': end_ = End::killed; return std::nullopt;
    case 'D': end_ = End::detached; return "OK";
    default: return "";
    }
}

std::string GdbServer::query(std::string_view packet) {
    if (packet.starts_with("qSupported")) {
        return std::format("PacketSize={};qXfer:features:read+;vContSupported+;"
                           "QStartNoAckMode+;ReverseStep+;ReverseContinue+",
                           k_packet_size);
    }
    if (packet == "QStartNoAckMode") {
        // This packet is still acknowledged; from the next one on, nothing is.
        acknowledging_ = false;
        return "OK";
    }
    if (packet == "qAttached") return "1";
    if (packet == "qC") return std::format("QC{:x}", stopped_thread_);
    if (packet == "qfThreadInfo") return thread_list();
    if (packet == "qsThreadInfo") return "l";
    if (packet.starts_with("qThreadExtraInfo,")) {
        const auto id = gdb::parse_number(packet.substr(17));
        const lv2::PpuThread* thread =
            id ? system_.kernel().find_thread(static_cast<u32>(*id)) : nullptr;
        if (!thread) return "E01";
        return gdb::to_hex(std::format("{}, {}", thread->name, status_name(thread->status)));
    }
    if (packet.starts_with("qXfer:features:read:target.xml:")) {
        const auto fields = split(packet.substr(31), ',');
        if (fields.size() != 2) return "E01";
        const auto offset = gdb::parse_number(fields[0]);
        const auto length = gdb::parse_number(fields[1]);
        const std::string_view xml = target_description();
        if (!offset || !length || *offset > xml.size()) return "E01";
        const std::string_view part = xml.substr(*offset, *length);
        return (*offset + part.size() < xml.size() ? "m" : "l") + std::string(part);
    }
    if (packet.starts_with("qRcmd,")) {
        const auto command = gdb::from_hex(packet.substr(6));
        if (!command) return "E01";
        const std::string output = monitor(std::string(command->begin(), command->end()));
        return output.empty() ? "OK" : gdb::to_hex(output);
    }
    if (packet.starts_with("qSymbol")) return "OK";
    return "";
}

// Runs until something stops the program, checking every k_poll_interval instructions
// whether GDB sent an interrupt (then the result is an instruction limit). Stops before `end`
// instructions in all, if it is set: that replays a stretch GDB has seen, which needs no new
// checkpoints. Otherwise takes checkpoints on the way.
RunResult GdbServer::run(RunOptions run, std::optional<u64> end) {
    while (true) {
        if (!end) take_checkpoint();
        run.max_instructions = std::min(k_poll_interval, checkpoint_interval_);
        if (end) {
            if (system_.instructions() >= *end) return RunResult{};
            run.max_instructions = std::min(run.max_instructions, *end - system_.instructions());
        }
        const RunResult result = system_.run(run);
        if (result.outcome != RunResult::Outcome::instruction_limit) return result;
        if (end) continue; // replaying a stretch GDB has already seen run

        while (const auto byte = connection_.read(false)) {
            const auto event = reader_.feed(*byte);
            if (event && event->kind == gdb::PacketReader::Event::Kind::interrupt) return result;
        }
    }
}

// Lets `thread` execute one instruction, though a breakpoint is on it.
RunResult GdbServer::step(u32 thread, std::optional<u64> end) {
    RunOptions run = options_;
    std::set<u64> others = breakpoints_;
    if (const lv2::PpuThread* stepped = system_.kernel().find_thread(thread)) {
        others.erase(stepped->cpu.state.pc);
    }
    run.breakpoints = &others;
    run.step_thread = thread;
    return this->run(run, end);
}

// Continues (stopping at breakpoints) or steps one thread.
std::string GdbServer::resume(std::optional<u32> step_thread) {
    if (step_thread) return stop_reply(step(*step_thread, std::nullopt));

    // A thread stopped on a breakpoint would hit it again at once: first it executes that
    // instruction.
    const lv2::PpuThread* stopped = system_.kernel().find_thread(stopped_thread_);
    if (stopped && breakpoints_.contains(stopped->cpu.state.pc)) {
        const RunResult result = step(stopped->id, std::nullopt);
        if (result.outcome != RunResult::Outcome::stepped) return stop_reply(result);
    }
    RunOptions options = options_;
    options.breakpoints = &breakpoints_;
    return stop_reply(run(options, std::nullopt));
}

// ---- Going back in time ----

// Saves the state, unless a checkpoint was saved less than checkpoint_interval_ ago.
void GdbServer::take_checkpoint() {
    const u64 now = system_.instructions();
    const auto later = checkpoints_.upper_bound(now);
    if (later != checkpoints_.begin() && now - std::prev(later)->first < checkpoint_interval_) {
        return;
    }
    checkpoints_[now] = system_.save_state();

    // Too many: drop every other one but the first, so they cover the run ever more thinly
    // the further back they go.
    while (checkpoints_.size() > k_max_checkpoints) {
        auto it = std::next(checkpoints_.begin());
        while (it != checkpoints_.end()) {
            it = checkpoints_.erase(it);
            if (it != checkpoints_.end()) ++it;
        }
    }
}

// GDB changed registers or memory. Replaying from an earlier checkpoint wouldn't repeat the
// change, so going back can't reach past this point any more.
void GdbServer::forget_checkpoints() {
    checkpoints_.clear();
    take_checkpoint();
}

// The checkpoints at or before the present, latest first.
std::vector<u64> GdbServer::checkpoints_before(u64 now) const {
    std::vector<u64> found;
    for (const auto& [instructions, state] : checkpoints_) {
        if (instructions <= now) found.insert(found.begin(), instructions);
    }
    return found;
}

std::string GdbServer::replay_begin() {
    if (!checkpoints_.empty()) system_.load_state(checkpoints_.begin()->second);
    update_stopped_thread();
    return std::format("T05thread:{:x};replaylog:begin;", stopped_thread_);
}

// Goes back to just before `thread`'s previous instruction: from the latest checkpoint before
// it, runs forward until the thread is about to execute it.
std::string GdbServer::reverse_step(u32 thread) {
    const lv2::PpuThread* current = system_.kernel().find_thread(thread);
    if (!current || current->executed == 0) return replay_begin();
    const RunOptions::ThreadPosition target{thread, current->executed - 1};

    for (const u64 checkpoint : checkpoints_before(system_.instructions())) {
        system_.load_state(checkpoints_.at(checkpoint));
        const lv2::PpuThread* then = system_.kernel().find_thread(thread);
        if (then && then->executed > target.executed) continue;
        RunOptions options = options_;
        options.stop_at = target;
        return stop_reply(run(options, std::nullopt));
    }
    return replay_begin();
}

// Goes back to the last breakpoint hit before the present. Each stretch between checkpoints is
// replayed, latest first, noting where breakpoints are hit; then the last hit is revisited.
std::string GdbServer::reverse_continue() {
    u64 end = system_.instructions();
    for (const u64 checkpoint : checkpoints_before(end)) {
        // A copy: going to the hit takes checkpoints, which may thin out this one.
        const std::vector<u8> start = checkpoints_.at(checkpoint);
        system_.load_state(start);
        std::optional<RunOptions::ThreadPosition> last_hit;
        RunOptions options = options_;
        options.breakpoints = &breakpoints_;
        while (system_.instructions() < end) {
            const RunResult result = run(options, end);
            if (result.outcome != RunResult::Outcome::breakpoint) break;
            const lv2::PpuThread& hit = *system_.kernel().find_thread(result.thread);
            last_hit = RunOptions::ThreadPosition{hit.id, hit.executed};
            if (step(hit.id, end).outcome != RunResult::Outcome::stepped) break;
        }
        if (last_hit) {
            system_.load_state(start);
            RunOptions back = options_;
            back.stop_at = last_hit;
            return stop_reply(run(back, std::nullopt));
        }
        end = checkpoint;
    }
    return replay_begin();
}

std::string GdbServer::stop_reply(const RunResult& result) {
    last_result_ = result;
    if (result.thread != 0) stopped_thread_ = result.thread;
    update_stopped_thread();

    // Signal numbers as GDB numbers them: SIGINT 2, SIGTRAP 5, SIGABRT 6, SIGSEGV 11.
    u32 signal = 5;
    switch (result.outcome) {
    case RunResult::Outcome::exited:
        end_ = End::program_ended;
        return std::format("W{:02x}", static_cast<u8>(result.exit_status));
    case RunResult::Outcome::faulted: signal = 11; break;
    case RunResult::Outcome::deadlocked: signal = 6; break;
    case RunResult::Outcome::instruction_limit: signal = 2; break;
    case RunResult::Outcome::breakpoint:
    case RunResult::Outcome::stepped:
    case RunResult::Outcome::reached: signal = 5; break;
    }
    // Faults and deadlocks explain themselves on GDB's console first.
    if (!result.message.empty()) {
        connection_.write(gdb::frame("O" + gdb::to_hex(result.message + "\n")));
    }
    return std::format("T{:02x}thread:{:x};", signal, stopped_thread_);
}

std::string GdbServer::thread_list() const {
    std::string list;
    for (const auto& thread : system_.kernel().threads()) {
        if (thread->status == lv2::ThreadStatus::finished) continue;
        list += std::format("{}{:x}", list.empty() ? "m" : ",", thread->id);
    }
    return list.empty() ? "l" : list;
}

lv2::PpuThread* GdbServer::selected_thread() {
    const u32 id = selected_thread_ != 0 && selected_thread_ != static_cast<u32>(-1)
                       ? selected_thread_
                       : stopped_thread_;
    return system_.kernel().find_thread(id);
}

std::string GdbServer::read_registers(const lv2::PpuThread& thread) const {
    std::string hex;
    for (u32 number = 0; number < k_register_count; ++number) {
        hex += *read_register(thread, number);
    }
    return hex;
}

bool GdbServer::write_registers(lv2::PpuThread& thread, std::string_view hex) {
    const auto bytes = gdb::from_hex(hex);
    if (!bytes) return false;
    std::size_t position = 0;
    for (u32 number = 0; number < k_register_count; ++number) {
        const u32 size = register_size(number);
        if (position + size > bytes->size()) return false;
        write_register(thread, number, std::span(*bytes).subspan(position, size));
        position += size;
    }
    return true;
}

std::optional<std::string> GdbServer::read_register(const lv2::PpuThread& thread,
                                                    u32 number) const {
    const ppu::State& state = thread.cpu.state;
    if (number < k_reg_f0) return hex_be(state.gpr[number - k_reg_r0], 8);
    if (number < k_reg_pc) return hex_be(state.fpr[number - k_reg_f0], 8);
    if (number >= k_reg_vr0 && number < k_reg_vscr) {
        return gdb::to_hex(state.vr[number - k_reg_vr0].bytes);
    }
    switch (number) {
    case k_reg_pc: return hex_be(state.pc, 8);
    case k_reg_msr: return hex_be(k_msr, 8);
    case k_reg_cr: return hex_be(state.cr, 4);
    case k_reg_lr: return hex_be(state.lr, 8);
    case k_reg_ctr: return hex_be(state.ctr, 8);
    case k_reg_xer: return hex_be(state.xer.value(), 4);
    case k_reg_fpscr: return hex_be(state.fpscr, 4);
    case k_reg_vscr: return hex_be(state.vscr, 4);
    case k_reg_vrsave: return hex_be(state.vrsave, 4);
    default: return std::nullopt;
    }
}

bool GdbServer::write_register(lv2::PpuThread& thread, u32 number, std::span<const u8> bytes) {
    if (number >= k_register_count || bytes.size() != register_size(number)) return false;
    ppu::State& state = thread.cpu.state;
    const u64 value = load_be_sized(bytes);
    if (number < k_reg_f0) {
        state.gpr[number - k_reg_r0] = value;
    } else if (number < k_reg_pc) {
        state.fpr[number - k_reg_f0] = value;
    } else if (number >= k_reg_vr0 && number < k_reg_vscr) {
        std::copy(bytes.begin(), bytes.end(), state.vr[number - k_reg_vr0].bytes.begin());
    } else {
        switch (number) {
        case k_reg_pc: state.pc = value; break;
        case k_reg_msr: break; // fixed
        case k_reg_cr: state.cr = static_cast<u32>(value); break;
        case k_reg_lr: state.lr = value; break;
        case k_reg_ctr: state.ctr = value; break;
        case k_reg_xer: state.xer.set(value); break;
        case k_reg_fpscr: state.fpscr = static_cast<u32>(value); break;
        case k_reg_vscr: state.vscr = static_cast<u32>(value); break;
        case k_reg_vrsave: state.vrsave = static_cast<u32>(value); break;
        default: return false;
        }
    }
    return true;
}

// Reads up to the first byte that isn't readable; an error if that is the first one.
std::string GdbServer::read_memory(u64 address, u64 length) const {
    std::vector<u8> bytes;
    const Memory& memory = system_.memory();
    for (u64 i = 0; i < std::min(length, k_max_memory_read); ++i) {
        if (!memory.is_accessible(address + i, 1, Memory::k_read)) break;
        bytes.push_back(memory.read<u8>(address + i));
    }
    if (bytes.empty() && length != 0) return "E14";
    return gdb::to_hex(bytes);
}

std::string GdbServer::monitor(std::string_view command) {
    const std::vector<std::string_view> words = split(command, ' ');
    const auto& spus = system_.kernel().spu_threads();
    auto find_spu = [&](std::string_view text) -> const lv2::SpuThread* {
        const auto index = parse_argument(text);
        if (!index || *index >= spus.size() || !spus[*index]->spe) return nullptr;
        return spus[*index].get();
    };

    if (words.empty() || words[0] == "help") {
        return "monitor spu                      list SPU threads\n"
               "monitor spu regs N               SPU thread N's registers\n"
               "monitor spu ls N ADDRESS LENGTH  bytes from SPU thread N's local store\n"
               "monitor save-state FILE          save the machine's state to FILE\n";
    }
    if (words[0] == "spu" && words.size() == 1) {
        if (spus.empty()) return "no SPU threads\n";
        std::string text;
        for (std::size_t i = 0; i < spus.size(); ++i) {
            const lv2::SpuThread& spu = *spus[i];
            text += std::format("{}: thread {:#x} \"{}\", {}", i, spu.id, spu.name,
                                status_name(spu.status));
            if (spu.spe) text += std::format(", pc {:#x}", spu.spe->cpu.state.pc);
            if (spu.blocked_channel)
                text += std::format(", waits on channel {}", *spu.blocked_channel);
            text += "\n";
        }
        return text;
    }
    if (words[0] == "spu" && words.size() == 3 && words[1] == "regs") {
        const lv2::SpuThread* spu = find_spu(words[2]);
        if (!spu) return "no such SPU thread; see 'monitor spu'\n";
        const spu::State& state = spu->spe->cpu.state;
        std::string text;
        for (u32 i = 0; i < state.gpr.size(); ++i) {
            const Vector& r = state.gpr[i];
            text += std::format("${:<4} {:08x} {:08x} {:08x} {:08x}\n", i, r.word(0), r.word(1),
                                r.word(2), r.word(3));
        }
        text += std::format("pc    {:#x}\nsrr0  {:#x}\n", state.pc, state.srr0);
        return text;
    }
    if (words[0] == "spu" && words.size() == 5 && words[1] == "ls") {
        const lv2::SpuThread* spu = find_spu(words[2]);
        const auto address = parse_argument(words[3]);
        const auto length = parse_argument(words[4]);
        if (!spu || !address || !length) return "usage: monitor spu ls N ADDRESS LENGTH\n";
        const spu::LocalStore& ls = spu->spe->cpu.local_store;
        std::string text;
        for (u64 offset = 0; offset < std::min<u64>(*length, spu::k_local_store_size);
             offset += 16) {
            const u32 line = static_cast<u32>(*address + offset) & spu::k_lslr;
            text += std::format("{:#07x}:", line);
            for (u64 i = offset; i < std::min<u64>(offset + 16, *length); ++i) {
                text += std::format(" {:02x}", ls.read<u8>(static_cast<u32>(*address + i)));
            }
            text += "\n";
        }
        return text;
    }
    if (words[0] == "save-state" && words.size() == 2) {
        const std::vector<u8> bytes = system_.save_state();
        std::ofstream file{std::string(words[1]), std::ios::binary};
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) return std::format("cannot write {}\n", words[1]);
        return std::format("saved {} bytes to {}\n", bytes.size(), words[1]);
    }
    return "unknown command; try 'monitor help'\n";
}

} // namespace debug
