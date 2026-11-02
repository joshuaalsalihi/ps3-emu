#include "core/memory.h"

#include "core/state.h"

#include <algorithm>
#include <cstring>
#include <format>

MemoryFault::MemoryFault(u64 address, bool is_write)
    : std::runtime_error(
          std::format("invalid memory {} at {:#x}", is_write ? "write" : "read", address)),
      address(address), is_write(is_write) {}

Memory::Memory()
    : host_(k_size), page_permissions_(k_size / k_page_size, 0),
      written_pages_(k_size / k_page_size, 0) {}

void Memory::map(u64 address, u64 size, u8 permissions) {
    set_permissions(address, size, permissions);
    // Only written pages can hold anything but zeros.
    const u64 first_page = address / k_page_size;
    zero_written_pages(first_page, first_page + size / k_page_size);
}

// The page loops here and in serialize() index raw arrays: they run over up to a million
// pages, and debug builds would otherwise make a function call per page.
void Memory::zero_written_pages(u64 first_page, u64 end_page) {
    u8* written = written_pages_.data();
    for (u64 page = first_page; page < end_page; ++page) {
        if (!written[page]) continue;
        std::memset(host_.data() + page * k_page_size, 0, k_page_size);
        written[page] = 0;
    }
}

void Memory::protect(u64 address, u64 size, u8 permissions) {
    set_permissions(address, size, permissions);
}

void Memory::unmap(u64 address, u64 size) {
    set_permissions(address, size, 0);
    lose_reservations(address, size);
}

void Memory::set_permissions(u64 address, u64 size, u8 permissions) {
    if (address % k_page_size != 0 || size % k_page_size != 0 || address > k_size ||
        size > k_size - address) {
        throw std::invalid_argument(std::format(
            "bad memory range {:#x} + {:#x}: must be page-aligned and below 4 GB", address, size));
    }
    const u64 first_page = address / k_page_size;
    const u64 page_count = size / k_page_size;
    std::fill_n(page_permissions_.begin() + first_page, page_count, permissions);
}

bool Memory::is_accessible(u64 address, u64 size, u8 permissions) const {
    if (size == 0) return true;
    if (address >= k_size || size > k_size - address) return false;

    const u64 first_page = address / k_page_size;
    const u64 last_page = (address + size - 1) / k_page_size;
    for (u64 page = first_page; page <= last_page; ++page) {
        if ((page_permissions_[page] & permissions) != permissions) return false;
    }
    return true;
}

void Memory::check_access(u64 address, u64 size, u8 permissions) const {
    if (!is_accessible(address, size, permissions)) {
        throw MemoryFault(address, permissions & k_write);
    }
}

void Memory::read_bytes(u64 address, std::span<u8> out) const {
    if (out.empty()) return;
    check_access(address, out.size(), k_read);
    std::memcpy(out.data(), host_.data() + address, out.size());
}

std::string Memory::read_string(u64 address, u64 max_length) const {
    std::string text;
    for (u64 i = 0; i < max_length; ++i) {
        const char c = static_cast<char>(read<u8>(address + i));
        if (c == '\0') break;
        text += c;
    }
    return text;
}

void Memory::write_bytes(u64 address, std::span<const u8> bytes) {
    if (bytes.empty()) return;
    check_access(address, bytes.size(), k_write);
    lose_reservations(address, bytes.size());
    const u64 first_page = address / k_page_size;
    const u64 last_page = (address + bytes.size() - 1) / k_page_size;
    std::fill(written_pages_.begin() + first_page, written_pages_.begin() + last_page + 1, 1);
    std::memcpy(host_.data() + address, bytes.data(), bytes.size());
}

bool Memory::debug_write(u64 address, std::span<const u8> bytes) {
    if (bytes.empty()) return true;
    if (address >= k_size || bytes.size() > k_size - address) return false;
    const u64 first_page = address / k_page_size;
    const u64 last_page = (address + bytes.size() - 1) / k_page_size;
    for (u64 page = first_page; page <= last_page; ++page) {
        if (page_permissions_[page] == 0) return false;
    }
    lose_reservations(address, bytes.size());
    std::fill(written_pages_.begin() + first_page, written_pages_.begin() + last_page + 1, 1);
    std::memcpy(host_.data() + address, bytes.data(), bytes.size());
    return true;
}

void Memory::reserve(u32 owner, u64 address) {
    clear_reservation(owner);
    reservations_.push_back({owner, address / k_line_size});
}

bool Memory::has_reservation(u32 owner, u64 address) const {
    return std::ranges::any_of(reservations_, [&](const Reservation& r) {
        return r.owner == owner && r.line == address / k_line_size;
    });
}

void Memory::clear_reservation(u32 owner) {
    std::erase_if(reservations_, [&](const Reservation& r) { return r.owner == owner; });
}

void Memory::lose_reservations(u64 address, u64 size) {
    if (reservations_.empty() || size == 0) return;

    const u64 first_line = address / k_line_size;
    const u64 last_line = (address + size - 1) / k_line_size;
    std::erase_if(reservations_, [&](const Reservation& r) {
        return r.line >= first_line && r.line <= last_line;
    });
}

void Memory::serialize(state::Archive& archive) {
    // Permissions, as runs of pages that share them.
    const u64 page_count = page_permissions_.size();
    const u8* permissions = page_permissions_.data();
    const u8* written = written_pages_.data();
    std::vector<std::pair<u8, u32>> runs;
    if (!archive.loading()) {
        u64 start = 0;
        for (u64 page = 1; page <= page_count; ++page) {
            if (page < page_count && permissions[page] == permissions[start]) continue;
            runs.push_back({permissions[start], static_cast<u32>(page - start)});
            start = page;
        }
    }
    archive(runs);

    // The contents of mapped pages that have been written; the rest hold zeros.
    std::vector<u32> pages;
    if (!archive.loading()) {
        for (u64 page = 0; page < page_count; ++page) {
            if (permissions[page] != 0 && written[page]) pages.push_back(static_cast<u32>(page));
        }
    }
    archive(pages);

    if (archive.loading()) {
        // Start from an empty address space.
        zero_written_pages(0, page_count);
        u64 page = 0;
        for (const auto& [permissions, count] : runs) {
            archive.expect(count <= page_permissions_.size() - page, "too many memory pages");
            std::fill_n(page_permissions_.begin() + page, count, permissions);
            page += count;
        }
        archive.expect(page == page_permissions_.size(), "too few memory pages");
    }
    for (const u32 page : pages) {
        archive.expect(page < page_permissions_.size() && page_permissions_[page] != 0,
                       "contents for an unmapped page");
        written_pages_[page] = 1;
        archive.raw(std::span(host_.data() + u64{page} * k_page_size, k_page_size));
    }

    std::vector<std::pair<u32, u64>> reservations;
    for (const Reservation& r : reservations_) {
        reservations.push_back({r.owner, r.line});
    }
    archive(reservations);
    if (archive.loading()) {
        reservations_.clear();
        for (const auto& [owner, line] : reservations) {
            reservations_.push_back({owner, line});
        }
    }
}
