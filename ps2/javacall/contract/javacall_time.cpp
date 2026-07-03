// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_time_* symbols the phoneME VM links against. Two groups:
//  - the wall/monotonic clock and sleep, delegated to hal::SystemClock;
//  - the native timer set (create/finalize/suspend/resume), delegated to
//    hal::TimerService. OS_javacall.cpp::start_ticks() arms the cyclic 30 ms
//    scheduler tick through javacall_time_initialize_timer, and its callback
//    calls real_time_tick(), so this timer is load-bearing, not optional.
//
// Thin shims only — no logic beyond marshalling.
#include "javacall_time.h"   // phoneME contract header (provided via -I at build)

#include "../hal/SystemClock.hpp"
#include "../hal/TimerService.hpp"

extern "C" {

// --- native timers -------------------------------------------------------

javacall_result javacall_time_initialize_timer(
        int                     wakeupInMilliSecondsFromNow,
        javacall_bool           cyclic,
        javacall_callback_func  func,
        /*OUT*/ javacall_handle* handle) {
    return ps2::hal::TimerService::instance().initialize(
        wakeupInMilliSecondsFromNow,
        cyclic == JAVACALL_TRUE,
        func,
        handle);
}

javacall_result javacall_time_finalize_timer(javacall_handle handle) {
    return ps2::hal::TimerService::instance().finalize(handle);
}

void javacall_time_suspend_ticks(javacall_handle handle) {
    ps2::hal::TimerService::instance().suspend(handle);
}

void javacall_time_resume_ticks(javacall_handle handle) {
    ps2::hal::TimerService::instance().resume(handle);
}

void javacall_time_sleep(javacall_uint64 ms) {
    ps2::hal::SystemClock::instance().sleep(ms);
}

// --- clocks --------------------------------------------------------------

javacall_int64 javacall_time_get_milliseconds_since_1970(void) {
    return ps2::hal::SystemClock::instance().wallClockMillis();
}

javacall_time_seconds javacall_time_get_seconds_since_1970(void) {
    return (javacall_time_seconds)ps2::hal::SystemClock::instance().wallClockSeconds();
}

javacall_time_milliseconds javacall_time_get_clock_milliseconds(void) {
    return (javacall_time_milliseconds)ps2::hal::SystemClock::instance().elapsedMillis();
}

javacall_int64 javacall_time_get_monotonic_clock_counter(void) {
    return ps2::hal::SystemClock::instance().monotonicCounter();
}

javacall_int64 javacall_time_get_monotonic_clock_frequency(void) {
    return ps2::hal::SystemClock::instance().monotonicFrequency();
}

char* javacall_time_get_local_timezone(void) {
    // The contract returns a non-const char*, but callers must not free or
    // modify it; SystemClock owns the storage. Cast away const at the boundary.
    return const_cast<char*>(ps2::hal::SystemClock::instance().localTimezone());
}

} // extern "C"
