// PS2 JavaCall port — HAL layer. MidletLifecycle implementation.
#include "MidletLifecycle.hpp"

// javacall_lifecycle.h defines the JAVACALL_LIFECYCLE_MIDLET_* state enum.
#include "javacall_lifecycle.h"

namespace ps2 {
namespace hal {

MidletLifecycle& MidletLifecycle::instance() {
    static MidletLifecycle inst;
    return inst;
}

void MidletLifecycle::notifyStateChanged(int state, javacall_result /*status*/) {
    lastState_ = state;
    if (state == JAVACALL_LIFECYCLE_MIDLET_SHUTDOWN) {
        shutdown_ = true;
    }
}

javacall_result MidletLifecycle::platformRequest(const char* /*url*/) {
    // No browser/dialer to hand the URL to: report the scheme as unsupported.
    return JAVACALL_CONNECTION_NOT_FOUND;
}

} // namespace hal
} // namespace ps2
