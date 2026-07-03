// PS2 JavaCall port — HAL layer. MemoryManager implementation.
#include "MemoryManager.hpp"
#include "IHeap.hpp"

#include <cstring>   // memset, memcpy, strlen

namespace ps2 {
namespace hal {

MemoryManager& MemoryManager::instance() {
    static MemoryManager inst;
    return inst;
}

void* MemoryManager::malloc(unsigned int size) {
    return heap_ != 0 ? heap_->allocate(size) : 0;
}

void* MemoryManager::realloc(void* ptr, unsigned int size) {
    return heap_ != 0 ? heap_->reallocate(ptr, size) : 0;
}

void MemoryManager::free(void* ptr) {
    if (heap_ != 0) {
        heap_->deallocate(ptr);
    }
}

void* MemoryManager::heapAllocate(long size, long* outSize) {
    void* ptr = (size > 0) ? malloc((unsigned int)size) : 0;
    if (outSize != 0) {
        *outSize = (ptr != 0) ? size : 0;
    }
    return ptr;
}

void MemoryManager::heapDeallocate(void* heap) {
    free(heap);
}

void* MemoryManager::calloc(unsigned int numberOfElements, unsigned int elementSize) {
    if (numberOfElements == 0 || elementSize == 0) {
        return malloc(0);
    }
    // Guard the multiplication against overflow before requesting the block.
    if (numberOfElements > 0xFFFFFFFFu / elementSize) {
        return 0;
    }
    const unsigned int total = numberOfElements * elementSize;
    void* ptr = malloc(total);
    if (ptr != 0) {
        std::memset(ptr, 0, total);
    }
    return ptr;
}

char* MemoryManager::strdup(const char* str) {
    if (str == 0) {
        return 0;
    }
    const unsigned int size = (unsigned int)std::strlen(str) + 1;
    char* copy = (char*)malloc(size);
    if (copy != 0) {
        std::memcpy(copy, str, size);
    }
    return copy;
}

} // namespace hal
} // namespace ps2
