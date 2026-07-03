// PS2 JavaCall port — platform layer. Ps2Clock implementation + registration.
#include "Ps2Clock.hpp"
#include "../hal/SystemClock.hpp"

#include <timer.h>   // GetTimerSystemTime, kBUSCLK, MSec2TimerBusClock

namespace ps2 {
namespace platform {

namespace {
// The EE has no battery-backed calendar reachable through newlib here, so anchor
// the wall clock at a fixed, plausible epoch and advance it with uptime. Good
// enough for System.currentTimeMillis ordering and elapsed-time math; a CDVD-RTC
// read can refine the absolute value later. 2026-07-03T00:00:00Z, ms since epoch.
const javacall_int64 WALL_EPOCH_MS = 1783036800000LL;

javacall_int64 busClockTicks() {
    return (javacall_int64)GetTimerSystemTime();
}
} // namespace

javacall_int64 Ps2Clock::monotonicCounter() {
    return busClockTicks();
}

javacall_int64 Ps2Clock::monotonicFrequency() {
    return (javacall_int64)kBUSCLK;   // 147,456,000 ticks/s
}

javacall_int64 Ps2Clock::wallClockMillis() {
    return WALL_EPOCH_MS + busClockTicks() * 1000 / (javacall_int64)kBUSCLK;
}

void Ps2Clock::sleep(javacall_uint64 ms) {
    if (ms == 0) {
        return;
    }
    // Busy-wait on the hardware timer. Interrupts (the 30 ms scheduler tick, vsync)
    // still fire during the spin, so the VM's time keeps advancing. A kernel-sleep
    // or semaphore wait can replace this later to stop burning EE cycles.
    const javacall_int64 start   = busClockTicks();
    const javacall_int64 waitFor = (javacall_int64)MSec2TimerBusClock(ms);
    while (busClockTicks() - start < waitFor) {
        /* spin */
    }
}

namespace {
// Register the PS2 hardware clock at startup, before the VM reads any time.
// Replaces PosixClockRegistrar (PosixClock.cpp is dropped from the PS2 build).
struct Ps2ClockRegistrar {
    Ps2Clock clock;
    Ps2ClockRegistrar() { hal::SystemClock::instance().setClock(&clock); }
};
Ps2ClockRegistrar g_ps2ClockRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
