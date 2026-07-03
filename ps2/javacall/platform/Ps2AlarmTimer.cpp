// PS2 JavaCall port — platform layer. Ps2AlarmTimer implementation + registration.
#include "Ps2AlarmTimer.hpp"
#include "../hal/Timer.hpp"

#include <kernel.h>   // SetAlarm / iSetAlarm / ReleaseAlarm, s32/u16

namespace ps2 {
namespace platform {

namespace {

// EE H-SYNC rate. The alarm counts scanlines; NTSC runs ~15734 lines/s (PAL is
// ~15625). Using the NTSC figure keeps the 30 ms tick within a fraction of a
// millisecond of target, which is well inside the scheduler's tolerance.
const int HSYNCS_PER_SECOND = 15734;

u16 msToHsync(int intervalMs) {
    long ticks = (long)intervalMs * HSYNCS_PER_SECOND / 1000;
    if (ticks < 1) {
        ticks = 1;              // never schedule a zero-length alarm
    }
    if (ticks > 0xFFFF) {
        ticks = 0xFFFF;         // clamp to the u16 alarm range (~4.1 s)
    }
    return (u16)ticks;
}

// C trampoline the EE kernel calls at interrupt time. 'common' is the Timer*.
void alarmTrampoline(s32 /*alarm_id*/, u16 /*time*/, void* common);

} // namespace

Ps2AlarmTimer::Ps2AlarmTimer() {
    for (int i = 0; i < hal::TimerService::MAX_TIMERS; ++i) {
        slots_[i].timer   = 0;
        slots_[i].alarmId = -1;
    }
}

Ps2AlarmTimer::Slot* Ps2AlarmTimer::find(hal::Timer* timer) {
    for (int i = 0; i < hal::TimerService::MAX_TIMERS; ++i) {
        if (slots_[i].timer == timer) {
            return &slots_[i];
        }
    }
    return 0;
}

Ps2AlarmTimer::Slot* Ps2AlarmTimer::findFree() {
    return find(0);
}

bool Ps2AlarmTimer::arm(hal::Timer* timer) {
    if (timer == 0) {
        return false;
    }
    Slot* slot = findFree();
    if (slot == 0) {
        return false;
    }
    s32 id = SetAlarm(msToHsync(timer->intervalMs()), alarmTrampoline, timer);
    if (id < 0) {
        return false;           // no free hardware alarm
    }
    slot->timer   = timer;
    slot->alarmId = id;
    return true;
}

void Ps2AlarmTimer::disarm(hal::Timer* timer) {
    Slot* slot = find(timer);
    if (slot == 0) {
        return;
    }
    if (slot->alarmId >= 0) {
        ReleaseAlarm(slot->alarmId);
    }
    slot->timer   = 0;
    slot->alarmId = -1;
}

void Ps2AlarmTimer::onAlarm(hal::Timer* timer) {
    // Interrupt context. Deliver the tick; if the timer is cyclic and still
    // live, re-arm the one-shot alarm with the interrupt-safe syscall and record
    // its new id so a later disarm() can cancel it.
    const bool reArm = timer->onFire();
    Slot* slot = find(timer);
    if (reArm) {
        s32 id = iSetAlarm(msToHsync(timer->intervalMs()), alarmTrampoline, timer);
        if (slot != 0) {
            slot->alarmId = id;
        }
    } else if (slot != 0) {
        // One-shot completed: the alarm already fired and freed itself.
        slot->timer   = 0;
        slot->alarmId = -1;
    }
}

namespace {
// Single backend instance, registered at startup. Defined here so the
// trampoline can route interrupts back into it.
Ps2AlarmTimer g_ps2AlarmTimer;

void alarmTrampoline(s32 /*alarm_id*/, u16 /*time*/, void* common) {
    g_ps2AlarmTimer.onAlarm(static_cast<hal::Timer*>(common));
}

struct Ps2AlarmTimerRegistrar {
    Ps2AlarmTimerRegistrar() {
        hal::TimerService::instance().setBackend(&g_ps2AlarmTimer);
    }
};
Ps2AlarmTimerRegistrar g_ps2AlarmTimerRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
