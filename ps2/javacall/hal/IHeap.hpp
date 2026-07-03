// PS2 JavaCall port — HAL layer.
//
// IHeap: the raw allocator abstraction behind MemoryManager. It exposes only
// the three primitive operations a heap must provide; the higher-level helpers
// the VM uses (calloc, strdup, the big VM heap block) are composed from these
// in MemoryManager, so a platform backend stays as small as possible. The PS2
// backend wraps newlib malloc; a host test can plug in a tracking allocator.
#ifndef PS2_JAVACALL_HAL_IHEAP_HPP
#define PS2_JAVACALL_HAL_IHEAP_HPP

namespace ps2 {
namespace hal {

class IHeap {
public:
    virtual ~IHeap() {}

    /// Allocate @p size bytes with suitable alignment, or return 0.
    virtual void* allocate(unsigned int size) = 0;

    /// Resize the block at @p ptr to @p size bytes. Returns the new block, or 0
    /// on failure (in which case @p ptr is left untouched, per realloc(3)).
    /// @p ptr may be 0, in which case this behaves like allocate().
    virtual void* reallocate(void* ptr, unsigned int size) = 0;

    /// Release a block previously returned by allocate()/reallocate(). Passing 0
    /// is a no-op.
    virtual void deallocate(void* ptr) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IHEAP_HPP
