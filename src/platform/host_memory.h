#pragma once

#include "common/types.h"

// A large block of zero-filled host memory. The OS only commits pages when they are first
// touched, so reserving the whole 4 GB guest address space is cheap.
class HostMemory {
public:
    explicit HostMemory(u64 size);
    ~HostMemory();

    HostMemory(const HostMemory&) = delete;
    HostMemory& operator=(const HostMemory&) = delete;

    u8* data() const { return data_; }
    u64 size() const { return size_; }

private:
    u8* data_ = nullptr;
    u64 size_ = 0;
};
