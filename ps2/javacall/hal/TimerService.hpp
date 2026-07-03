// PS2 JavaCall port — HAL layer.
//
// TimerService: the service the javacall_time timer functions delegate to. It
// owns a small fixed pool of Timer control blocks (no dynamic allocation — new
// timers are only created from thread context, never from an interrupt) and
// drives them through an injected ITimerBackend. For Milestone A exactly one
// timer is live: the cyclic 30 ms scheduler tick that Os::start_ticks arms and
// that pumps real_time_tick(), the heartbeat of the green-thread scheduler.
#ifndef PS2_JAVACALL_HAL_TIMERSERVICE_HPP
#define PS2_JAVACALL_HAL_TIMERSERVICE_HPP

#include <javacall_defs.h>
#include <javacall_time.h>   // javacall_callback_func

#include "Timer.hpp"

namespace ps2 {
namespace hal {

class ITimerBackend;

class TimerService {
public:
    /// Maximum number of concurrently active native timers. The CLDC VM needs
    /// one (the scheduler tick); a few spare slots cover MIDP push/alarm use.
    static const int MAX_TIMERS = 8;

    static TimerService& instance();

    /// Install the hardware backend. Ownership stays with the caller (a static
    /// platform object). Must be set before the first initialize() call.
    void setBackend(ITimerBackend* backend) { backend_ = backend; }
    ITimerBackend* backend() const { return backend_; }

    /// Create and arm a native timer — javacall_time_initialize_timer. Returns
    /// the new timer's handle in @p out, or NULL on failure (no free slot / no
    /// backend). @p cyclic true repeats every @p intervalMs; false fires once.
    javacall_result initialize(int intervalMs, bool cyclic,
                               javacall_callback_func func,
                               javacall_handle* out);

    /// Disarm and release a timer — javacall_time_finalize_timer.
    javacall_result finalize(javacall_handle handle);

    /// Temporarily stop / resume callback delivery without tearing down the
    /// timer — javacall_time_suspend_ticks / javacall_time_resume_ticks.
    void suspend(javacall_handle handle);
    void resume(javacall_handle handle);

private:
    TimerService() : backend_(0) {}
    TimerService(const TimerService&);
    TimerService& operator=(const TimerService&);

    Timer* allocate();
    Timer* fromHandle(javacall_handle handle);

    ITimerBackend* backend_;
    Timer          pool_[MAX_TIMERS];
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_TIMERSERVICE_HPP
