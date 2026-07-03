// PS2 JavaCall port — platform layer. SystemHeap implementation + registration.
#include "SystemHeap.hpp"
#include "../hal/MemoryManager.hpp"

#include <cstdlib>   // std::malloc, std::realloc, std::free

namespace ps2 {
namespace platform {

void* SystemHeap::allocate(unsigned int size) {
    return std::malloc(size);
}

void* SystemHeap::reallocate(void* ptr, unsigned int size) {
    return std::realloc(ptr, size);
}

void SystemHeap::deallocate(void* ptr) {
    std::free(ptr);
}

namespace {
// Install the default heap at program startup, before the VM's first
// allocation. Kept in the platform layer so the HAL stays hardware-free.
struct SystemHeapRegistrar {
    SystemHeap heap;
    SystemHeapRegistrar() { hal::MemoryManager::instance().setHeap(&heap); }
};
SystemHeapRegistrar g_systemHeapRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
