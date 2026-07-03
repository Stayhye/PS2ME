// PS2 JavaCall port — HAL layer.
//
// MemoryManager: the memory service the javacall_memory contract delegates to.
// The VM asks for one large heap block once at startup (which it sub-allocates
// internally) plus small allocations for javacall's own use. MemoryManager
// routes all of these to an injected IHeap and implements the composite helpers
// (calloc, strdup) portably on top of the primitive allocate/free, so backends
// stay minimal.
#ifndef PS2_JAVACALL_HAL_MEMORYMANAGER_HPP
#define PS2_JAVACALL_HAL_MEMORYMANAGER_HPP

namespace ps2 {
namespace hal {

class IHeap;

class MemoryManager {
public:
    static MemoryManager& instance();

    /// Install the raw allocator. Ownership stays with the caller (a static
    /// platform object). With none installed, every allocation returns 0.
    void setHeap(IHeap* heap) { heap_ = heap; }
    IHeap* heap() const { return heap_; }

    // --- the big VM heap (allocated once at startup, freed once at shutdown) ---

    /// Allocate the VM's main heap of @p size bytes. On success writes the
    /// granted size to @p outSize; on failure returns 0 and writes 0.
    void* heapAllocate(long size, long* outSize);
    void  heapDeallocate(void* heap);

    // --- general javacall allocations ---

    void* malloc(unsigned int size);
    void* realloc(void* ptr, unsigned int size);
    void  free(void* ptr);

    /// Allocate numberOfElements*elementSize bytes, zero-filled. Returns 0 on
    /// overflow or allocation failure.
    void* calloc(unsigned int numberOfElements, unsigned int elementSize);

    /// Allocate a copy of the NUL-terminated @p str. Returns 0 on failure or if
    /// @p str is 0.
    char* strdup(const char* str);

private:
    MemoryManager() : heap_(0) {}
    MemoryManager(const MemoryManager&);
    MemoryManager& operator=(const MemoryManager&);

    IHeap* heap_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_MEMORYMANAGER_HPP
