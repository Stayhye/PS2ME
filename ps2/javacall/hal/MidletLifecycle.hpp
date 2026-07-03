// PS2 JavaCall port — HAL layer.
//
// MidletLifecycle: tracks the running MIDlet's lifecycle state as reported by the
// VM through javacall_lifecycle_state_changed, and answers platform requests. It
// is the place the entry point consults to know when the MIDlet has shut down, and
// where a future implementation will react to pause/resume (e.g. releasing the GS).
#ifndef PS2_JAVACALL_HAL_MIDLETLIFECYCLE_HPP
#define PS2_JAVACALL_HAL_MIDLETLIFECYCLE_HPP

#include <javacall_defs.h>

namespace ps2 {
namespace hal {

class MidletLifecycle {
public:
    static MidletLifecycle& instance();

    /// javacall_lifecycle_state_changed: record the new MIDlet state.
    void notifyStateChanged(int state, javacall_result status);

    /// True once the VM has reported the MIDlet reached the DESTROYED state.
    bool isShutdown() const { return shutdown_; }

    int lastState() const { return lastState_; }

    /// javacall_lifecycle_platform_request: PS2 has no external URL handler.
    javacall_result platformRequest(const char* url);

private:
    MidletLifecycle() : lastState_(0), shutdown_(false) {}
    MidletLifecycle(const MidletLifecycle&);
    MidletLifecycle& operator=(const MidletLifecycle&);

    int  lastState_;
    bool shutdown_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_MIDLETLIFECYCLE_HPP
