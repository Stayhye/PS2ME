// PS2 JavaCall port — HAL layer.
//
// ICpuCache: the abstraction for CPU cache maintenance the VM needs when it
// relocates or rewrites executable code (JIT deoptimization, GC of compiled
// methods). Defining it here keeps OsCore free of hardware knowledge: the
// platform layer supplies a concrete cache (EE syscall on PS2, a no-op on a
// host test) without the HAL depending on ps2sdk.
#ifndef PS2_JAVACALL_HAL_ICPUCACHE_HPP
#define PS2_JAVACALL_HAL_ICPUCACHE_HPP

namespace ps2 {
namespace hal {

class ICpuCache {
public:
    virtual ~ICpuCache() {}

    /// Make instruction fetches in [address, address+size) observe the latest
    /// stores to that range. Implementations may flush more than requested
    /// (e.g. the whole instruction cache); over-flushing is always safe.
    virtual void flushInstructionCache(void* address, int size) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_ICPUCACHE_HPP
