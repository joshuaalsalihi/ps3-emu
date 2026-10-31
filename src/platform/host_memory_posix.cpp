#include "platform/host_memory.h"

#include <new>
#include <sys/mman.h>

HostMemory::HostMemory(u64 size) : size_(size) {
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        throw std::bad_alloc();
    }
    data_ = static_cast<u8*>(memory);
}

HostMemory::~HostMemory() {
    munmap(data_, size_);
}
