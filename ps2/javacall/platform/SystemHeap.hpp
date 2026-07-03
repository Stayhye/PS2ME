// PS2 JavaCall port — platform layer.
//
// SystemHeap: an IHeap backed by the C library allocator (malloc/realloc/free).
// On the PS2 this is ps2sdk's newlib heap, which grows the EE main-memory arena
// via sbrk; the same code links and runs on the host, so this single backend
// serves both builds (like the logging StdoutSink). A custom pooled/region
// allocator can replace it later without touching the HAL or contract layers.
#ifndef PS2_JAVACALL_PLATFORM_SYSTEMHEAP_HPP
#define PS2_JAVACALL_PLATFORM_SYSTEMHEAP_HPP

#include "../hal/IHeap.hpp"

namespace ps2 {
namespace platform {

class SystemHeap : public hal::IHeap {
public:
    virtual void* allocate(unsigned int size);
    virtual void* reallocate(void* ptr, unsigned int size);
    virtual void  deallocate(void* ptr);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_SYSTEMHEAP_HPP
