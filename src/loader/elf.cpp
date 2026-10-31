#include "loader/elf.h"

#include "common/endian.h"

#include <algorithm>
#include <format>

namespace loader {

namespace {

// Layout constants from the System V ABI, "ELF Header" and "Program Header".
constexpr u8 k_elf_magic[] = {0x7f, 'E', 'L', 'F'};
constexpr u8 k_self_magic[] = {'S', 'C', 'E', 0};
constexpr u8 k_class_64 = 2;
constexpr u8 k_data_big_endian = 2;
constexpr u64 k_header_size = 64;
constexpr u64 k_program_header_size = 56;

// Bounds-checked big-endian reads from the file.
class Reader {
public:
    explicit Reader(std::span<const u8> data) : data_(data) {}

    template <std::unsigned_integral T>
    T read(u64 offset) const {
        if (!contains(offset, sizeof(T))) {
            throw LoadError("file is truncated");
        }
        return load_be<T>(data_.data() + offset);
    }

    bool contains(u64 offset, u64 size) const {
        return offset <= data_.size() && size <= data_.size() - offset;
    }

    bool starts_with(std::span<const u8> prefix) const {
        return data_.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), data_.begin());
    }

private:
    std::span<const u8> data_;
};

void check_identity(const Reader& file) {
    if (file.starts_with(k_self_magic)) {
        throw LoadError("this is a SELF file; only plain ELF files are supported so far");
    }
    if (!file.starts_with(k_elf_magic)) {
        throw LoadError("not an ELF file");
    }
    if (file.read<u8>(4) != k_class_64) {
        throw LoadError("only 64-bit ELF files are supported");
    }
    if (file.read<u8>(5) != k_data_big_endian) {
        throw LoadError("expected a big-endian ELF file");
    }
}

ElfSegment read_segment(const Reader& file, u64 offset) {
    ElfSegment segment;
    segment.type = file.read<u32>(offset + 0);
    segment.flags = file.read<u32>(offset + 4);
    segment.file_offset = file.read<u64>(offset + 8);
    segment.vaddr = file.read<u64>(offset + 16);
    // offset + 24 is the physical address, which the PS3 does not use.
    segment.file_size = file.read<u64>(offset + 32);
    segment.mem_size = file.read<u64>(offset + 40);
    segment.align = file.read<u64>(offset + 48);
    return segment;
}

void check_segment(const Reader& file, const ElfSegment& segment, std::size_t index) {
    if (!file.contains(segment.file_offset, segment.file_size)) {
        throw LoadError(std::format("segment {} extends past the end of the file", index));
    }
    if (segment.file_size > segment.mem_size) {
        throw LoadError(std::format("segment {} is larger in the file than in memory", index));
    }
}

} // namespace

ElfFile parse_elf(std::span<const u8> data) {
    const Reader file(data);
    if (!file.contains(0, k_header_size)) {
        throw LoadError("file is too small to be an ELF file");
    }
    check_identity(file);

    ElfFile elf;
    elf.type = file.read<u16>(16);
    elf.machine = file.read<u16>(18);
    elf.entry = file.read<u64>(24);

    if (elf.machine != k_elf_machine_ppc64) {
        throw LoadError(
            std::format("expected a PowerPC 64 (PPU) file, got machine type {}", elf.machine));
    }

    const u64 table_offset = file.read<u64>(32);
    const u16 entry_size = file.read<u16>(54);
    const u16 entry_count = file.read<u16>(56);

    if (entry_count > 0 && entry_size != k_program_header_size) {
        throw LoadError(std::format("unexpected program header size {}", entry_size));
    }
    if (!file.contains(table_offset, u64{entry_count} * k_program_header_size)) {
        throw LoadError("program header table extends past the end of the file");
    }

    for (u16 i = 0; i < entry_count; ++i) {
        const ElfSegment segment = read_segment(file, table_offset + i * k_program_header_size);
        check_segment(file, segment, i);
        elf.segments.push_back(segment);
    }
    return elf;
}

std::string elf_type_name(u16 type) {
    switch (type) {
    case k_elf_type_exec: return "EXEC (executable)";
    case k_elf_type_dyn: return "DYN (shared object)";
    case k_elf_type_prx: return "PRX (PS3 library)";
    default: return std::format("unknown ({:#06x})", type);
    }
}

std::string segment_type_name(u32 type) {
    switch (type) {
    case k_segment_null: return "NULL";
    case k_segment_load: return "LOAD";
    case k_segment_dynamic: return "DYNAMIC";
    case k_segment_interp: return "INTERP";
    case k_segment_note: return "NOTE";
    case k_segment_phdr: return "PHDR";
    case k_segment_tls: return "TLS";
    case k_segment_proc_param: return "PROC_PARAM";
    case k_segment_prx_param: return "PRX_PARAM";
    case k_segment_ppu_rela: return "PPU_RELA";
    default: return std::format("{:#010x}", type);
    }
}

std::string segment_flags_string(u32 flags) {
    std::string result = "---";
    if (flags & k_segment_flag_r) result[0] = 'R';
    if (flags & k_segment_flag_w) result[1] = 'W';
    if (flags & k_segment_flag_x) result[2] = 'X';
    return result;
}

std::string describe_elf(const ElfFile& elf) {
    std::string out;
    out += std::format("Type:    {}\n", elf_type_name(elf.type));
    out += std::format("Entry:   {:#010x} (function descriptor)\n", elf.entry);
    out += std::format("\nSegments ({}):\n", elf.segments.size());
    out += std::format("  {:<3} {:<12} {:<12} {:<12} {:<12} {:<12} {}\n", "#", "Type", "Offset",
                       "Address", "File size", "Mem size", "Flags");

    for (std::size_t i = 0; i < elf.segments.size(); ++i) {
        const ElfSegment& s = elf.segments[i];
        out += std::format("  {:<3} {:<12} {:<#12x} {:<#12x} {:<#12x} {:<#12x} {}\n", i,
                           segment_type_name(s.type), s.file_offset, s.vaddr, s.file_size,
                           s.mem_size, segment_flags_string(s.flags));
    }
    return out;
}

} // namespace loader
