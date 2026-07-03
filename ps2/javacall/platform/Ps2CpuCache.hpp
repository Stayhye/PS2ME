// PS2 JavaCall port — platform layer.
//
// Ps2CpuCache: instruction-cache maintenance on the Emotion Engine via the
// ps2sdk FlushCache syscall. The EE has separate data and instruction caches,
// so making freshly written code visible to the fetch unit means writing the
// data cache back to memory and then invalidating the instruction cache.
#ifndef PS2_JAVACALL_PLATFORM_PS2CPUCACHE_HPP
#define PS2_JAVACALL_PLATFORM_PS2CPUCACHE_HPP

#include "../hal/ICpuCache.hpp"

namespace ps2 {
namespace platform {

class Ps2CpuCache : public hal::ICpuCache {
public:
    virtual void flushInstructionCache(void* address, int size);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2CPUCACHE_HPP
