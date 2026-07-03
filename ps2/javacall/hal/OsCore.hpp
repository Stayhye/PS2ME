// PS2 JavaCall port — HAL layer.
//
// OsCore: the OS-services facade the VM's Os porting layer talks to through the
// javacall_os contract. It owns two responsibilities that OS_javacall.cpp maps
// onto: process lifecycle (initialize/dispose, invoked once at VM start/stop)
// and instruction-cache maintenance (flush_icache). Neither touches hardware
// directly — cache work is delegated to an injected ICpuCache so the same logic
// runs on real PS2 hardware or under a host test with a no-op cache.
#ifndef PS2_JAVACALL_HAL_OSCORE_HPP
#define PS2_JAVACALL_HAL_OSCORE_HPP

namespace ps2 {
namespace hal {

class ICpuCache;

class OsCore {
public:
    /// Process-wide instance. Constructed on first use (safe static init order
    /// with the platform objects that register themselves at startup).
    static OsCore& instance();

    /// Install the CPU-cache backend. Ownership stays with the caller (typically
    /// a static platform object). Passing nullptr disables cache maintenance
    /// (flushInstructionCache becomes a safe no-op).
    void setCpuCache(ICpuCache* cache) { cpuCache_ = cache; }
    ICpuCache* cpuCache() const { return cpuCache_; }

    /// One-time OS bring-up, called from Os::initialize() at VM startup. The
    /// periodic scheduler tick is armed separately through TimerService, so for
    /// Milestone A there is nothing mandatory to do here; kept as the hook where
    /// future platform-wide init (interrupt setup, I/O) will live.
    void initialize();

    /// Reverse of initialize(), called from Os::dispose() at VM shutdown.
    void dispose();

    /// Flush the instruction cache for [address, address+size). No-op when no
    /// cache backend is installed or the arguments are degenerate.
    void flushInstructionCache(void* address, int size);

private:
    OsCore() : cpuCache_(0), initialized_(false) {}
    OsCore(const OsCore&);            // non-copyable
    OsCore& operator=(const OsCore&);

    ICpuCache* cpuCache_;
    bool       initialized_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_OSCORE_HPP
