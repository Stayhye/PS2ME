// PS2 JavaCall port — HAL layer. OsCore implementation.
#include "OsCore.hpp"
#include "ICpuCache.hpp"

namespace ps2 {
namespace hal {

OsCore& OsCore::instance() {
    // Function-local static: constructed on first call, no static-init-order
    // issue with the platform cache that registers itself at program startup.
    static OsCore inst;
    return inst;
}

void OsCore::initialize() {
    // Idempotent: the VM calls Os::initialize() once, but guard anyway so a
    // restart (dispose + initialize) stays well defined.
    if (initialized_) {
        return;
    }
    initialized_ = true;
}

void OsCore::dispose() {
    initialized_ = false;
}

void OsCore::flushInstructionCache(void* address, int size) {
    if (cpuCache_ != 0 && address != 0 && size > 0) {
        cpuCache_->flushInstructionCache(address, size);
    }
}

} // namespace hal
} // namespace ps2
