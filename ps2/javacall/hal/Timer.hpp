// PS2 JavaCall port — HAL layer.
//
// Timer: one native timer's control block, owned by TimerService. It carries
// the policy (interval, cyclic vs one-shot, suspended) so the platform backend
// only has to deal with the hardware; when the hardware fires it calls
// onFire(), which applies the policy and invokes the VM callback. A Timer's
// address is used as its javacall_handle, so the same object identifies the
// timer to the VM, the service, and the backend.
#ifndef PS2_JAVACALL_HAL_TIMER_HPP
#define PS2_JAVACALL_HAL_TIMER_HPP

#include <javacall_defs.h>
#include <javacall_time.h>   // javacall_callback_func

namespace ps2 {
namespace hal {

class Timer {
public:
    Timer() : func_(0), intervalMs_(0), cyclic_(false), suspended_(false), inUse_(false) {}

    // --- configuration, set by TimerService before the backend arms it ---
    void configure(int intervalMs, bool cyclic, javacall_callback_func func) {
        intervalMs_ = intervalMs;
        cyclic_     = cyclic;
        func_       = func;
        suspended_  = false;
        inUse_      = true;
    }
    void release() { inUse_ = false; func_ = 0; }

    int  intervalMs() const { return intervalMs_; }
    bool cyclic()     const { return cyclic_; }
    bool inUse()      const { return inUse_; }

    void suspend() { suspended_ = true; }
    void resume()  { suspended_ = false; }

    /// The javacall_handle the VM sees for this timer.
    javacall_handle handle() { return static_cast<javacall_handle>(this); }

    /// Called by the platform backend when the underlying hardware fires
    /// (typically in interrupt context). Delivers the VM callback unless the
    /// timer is suspended, and reports whether the backend should re-arm it
    /// (true for a still-active cyclic timer).
    bool onFire() {
        if (!suspended_ && func_ != 0) {
            func_(handle());
        }
        return cyclic_ && inUse_;
    }

private:
    javacall_callback_func func_;
    int                    intervalMs_;
    bool                   cyclic_;
    volatile bool          suspended_;   // toggled outside interrupt, read inside
    bool                   inUse_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_TIMER_HPP
