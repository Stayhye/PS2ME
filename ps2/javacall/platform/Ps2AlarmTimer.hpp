// PS2 JavaCall port — platform layer.
//
// Ps2AlarmTimer: the ITimerBackend for the PS2 target, built on the Emotion
// Engine kernel Alarm service (SetAlarm/ReleaseAlarm). EE alarms are one-shot
// and fire in interrupt context, counted in H-SYNC ticks; this backend converts
// the timer's millisecond interval to H-SYNCs, and re-arms cyclic timers from
// inside the interrupt handler (via the i-prefixed, interrupt-safe syscalls).
// It backs the 30 ms scheduler tick that pumps real_time_tick().
#ifndef PS2_JAVACALL_PLATFORM_PS2ALARMTIMER_HPP
#define PS2_JAVACALL_PLATFORM_PS2ALARMTIMER_HPP

#include "../hal/ITimerBackend.hpp"
#include "../hal/TimerService.hpp"   // MAX_TIMERS

namespace ps2 {
namespace platform {

class Ps2AlarmTimer : public hal::ITimerBackend {
public:
    Ps2AlarmTimer();

    virtual bool arm(hal::Timer* timer);
    virtual void disarm(hal::Timer* timer);

    // Called from the EE alarm interrupt (via the C trampoline) for the timer
    // whose control block is passed as the alarm's 'common' argument.
    void onAlarm(hal::Timer* timer);

private:
    // One row per potentially-active timer: which Timer it is and the live EE
    // alarm id, so disarm() and re-arm can find it. Indexed in lock-step with
    // TimerService's pool size.
    struct Slot {
        hal::Timer* timer;
        int         alarmId;   // valid when timer != 0
    };
    Slot slots_[hal::TimerService::MAX_TIMERS];

    Slot* find(hal::Timer* timer);
    Slot* findFree();
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2ALARMTIMER_HPP
