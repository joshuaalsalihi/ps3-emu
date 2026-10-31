#include "core/memory.h"

#include <algorithm>
#include <cstring>
#include <format>

MemoryFault::MemoryFault(u64 address, bool is_write)
    : std::runtime_error(
          std::format("invalid memory {} at {:#x}", is_write ? "write" : "read", address)),
      address(address), is_write(is_write) {}

Memory::Memory() : host_(k_size), page_permissions_(k_size / k_page_size, 0) {}

void Memory::map(u64 address, u64 size, u8 permissions) {
    set_permissions(address, size, permissions);
    std::memset(host_.data() + address, 0, size);
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
    std::memcpy(host_.data() + address, bytes.data(), bytes.size());
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
