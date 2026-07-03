// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_* memory symbols the phoneME VM links against: the big VM
// heap (allocated once at startup, freed at shutdown) and the general
// malloc/realloc/free/calloc/strdup family for javacall's own allocations. Thin
// shims — all behaviour lives in hal::MemoryManager.
#include "javacall_memory.h"   // phoneME contract header (provided via -I at build)

#include "../hal/MemoryManager.hpp"

extern "C" {

void* javacall_memory_heap_allocate(long size, /*OUT*/ long* outSize) {
    return ps2::hal::MemoryManager::instance().heapAllocate(size, outSize);
}

void javacall_memory_heap_deallocate(void* heap) {
    ps2::hal::MemoryManager::instance().heapDeallocate(heap);
}

void* javacall_malloc(unsigned int size) {
    return ps2::hal::MemoryManager::instance().malloc(size);
}

void* javacall_realloc(void* ptr, unsigned int size) {
    return ps2::hal::MemoryManager::instance().realloc(ptr, size);
}

void javacall_free(void* ptr) {
    ps2::hal::MemoryManager::instance().free(ptr);
}

void* javacall_calloc(unsigned int numberOfElements, unsigned int elementSize) {
    return ps2::hal::MemoryManager::instance().calloc(numberOfElements, elementSize);
}

char* javacall_strdup(const char* str) {
    return ps2::hal::MemoryManager::instance().strdup(str);
}

} // extern "C"
