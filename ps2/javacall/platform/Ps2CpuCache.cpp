// PS2 JavaCall port — platform layer. Ps2CpuCache implementation + registration.
#include "Ps2CpuCache.hpp"
#include "../hal/OsCore.hpp"

#include <kernel.h>   // ps2sdk EE kernel: FlushCache + cache mode constants

namespace ps2 {
namespace platform {

void Ps2CpuCache::flushInstructionCache(void* /*address*/, int /*size*/) {
    // The EE FlushCache syscall operates on the whole cache, not a range, so the
    // address/size hint is ignored — over-flushing is safe. Two steps are
    // needed: push any dirty lines of the just-written code from the data cache
    // out to main memory (WRITEBACK_DCACHE), then drop stale lines from the
    // instruction cache so the fetch unit reloads the new code (INVALIDATE_ICACHE).
    FlushCache(WRITEBACK_DCACHE);
    FlushCache(INVALIDATE_ICACHE);
}

namespace {
// Install the EE cache backend at program startup, before the VM's first
// Os::flush_icache. Kept in the platform layer so the HAL stays hardware-free.
struct Ps2CpuCacheRegistrar {
    Ps2CpuCache cache;
    Ps2CpuCacheRegistrar() { hal::OsCore::instance().setCpuCache(&cache); }
};
Ps2CpuCacheRegistrar g_ps2CpuCacheRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
