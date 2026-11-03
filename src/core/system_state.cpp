// Save states: the whole machine as bytes, and back.

#include "core/system.h"

#include <array>
#include <string>

namespace {

// A state starts with these bytes, then the format version. A state from another version is
// rejected rather than misread.
constexpr std::array<u8, 8> k_magic = {'P', 'S', '3', 'S', 'T', 'A', 'T', 'E'};
constexpr u32 k_version = 2;

} // namespace

std::vector<u8> System::save_state() {
    state::Archive archive;
    serialize(archive);
    return archive.take_bytes();
}

void System::load_state(std::span<const u8> bytes) {
    state::Archive archive(bytes);
    serialize(archive);
    if (!archive.at_end()) throw state::Error("save state has extra bytes at the end");
}

void System::serialize(state::Archive& archive) {
    std::array<u8, 8> magic = k_magic;
    archive.raw(magic);
    if (magic != k_magic) throw state::Error("not a save state");
    u32 version = k_version;
    archive(version);
    if (version != k_version) {
        throw state::Error("save state is version " + std::to_string(version) +
                           ", but this build reads version " + std::to_string(k_version));
    }

    archive(clock_, memory_, kernel_, linker_, rsx_, instructions_);

    // The turn in progress, by thread ID.
    bool has_turn = turn_.has_value();
    archive(has_turn);
    if (!has_turn) {
        turn_.reset();
        return;
    }
    bool is_spu = !archive.loading() && turn_->spu;
    u32 id = archive.loading() ? 0 : (is_spu ? turn_->spu->id : turn_->ppu->id);
    u64 remaining = archive.loading() ? 0 : turn_->remaining;
    archive(is_spu, id, remaining);
    if (!archive.loading()) return;

    Turn turn{nullptr, nullptr, remaining};
    if (is_spu) {
        turn.spu = kernel_.find_spu_thread(id);
    } else {
        turn.ppu = kernel_.find_thread(id);
    }
    archive.expect(turn.ppu || turn.spu, "the turn belongs to no thread");
    turn_ = turn;
}
