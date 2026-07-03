// PS2 JavaCall port — HAL layer.
//
// ITimerBackend: the hardware side of the periodic timer, behind TimerService.
// It only starts and stops the underlying timer for a given Timer object; all
// policy (cyclic, suspended, which callback) lives in Timer/TimerService. When
// the hardware expires the backend must call Timer::onFire() and, if that
// returns true, re-arm the same Timer (one-shot hardware repeats itself this
// way; the PS2 alarm is one-shot).
#ifndef PS2_JAVACALL_HAL_ITIMERBACKEND_HPP
#define PS2_JAVACALL_HAL_ITIMERBACKEND_HPP

namespace ps2 {
namespace hal {

class Timer;

class ITimerBackend {
public:
    virtual ~ITimerBackend() {}

    /// Start the hardware so it will fire after timer->intervalMs(), then call
    /// timer->onFire(). Returns false if no hardware timer could be allocated.
    virtual bool arm(Timer* timer) = 0;

    /// Stop the hardware associated with @p timer. Safe to call on a timer that
    /// is not currently armed.
    virtual void disarm(Timer* timer) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_ITIMERBACKEND_HPP
