// PS2 JavaCall port — HAL layer. SystemClock implementation.
#include "SystemClock.hpp"
#include "IClock.hpp"

namespace ps2 {
namespace hal {

SystemClock& SystemClock::instance() {
    static SystemClock inst;
    return inst;
}

javacall_int64 SystemClock::wallClockMillis() {
    return clock_ != 0 ? clock_->wallClockMillis() : 0;
}

javacall_int64 SystemClock::wallClockSeconds() {
    return wallClockMillis() / 1000;
}

javacall_int64 SystemClock::monotonicCounter() {
    return clock_ != 0 ? clock_->monotonicCounter() : 0;
}

javacall_int64 SystemClock::monotonicFrequency() {
    // Never report 0: callers divide by this. Default to a 1 kHz (millisecond)
    // scale when no source is installed.
    if (clock_ != 0) {
        const javacall_int64 f = clock_->monotonicFrequency();
        if (f > 0) {
            return f;
        }
    }
    return 1000;
}

javacall_int64 SystemClock::elapsedMillis() {
    // Convert the raw monotonic counter to milliseconds. Multiply before
    // dividing to keep resolution; the counter is 64-bit so overflow is a
    // non-issue for any realistic uptime.
    return monotonicCounter() * 1000 / monotonicFrequency();
}

void SystemClock::sleep(javacall_uint64 ms) {
    if (clock_ != 0) {
        clock_->sleep(ms);
    }
}

const char* SystemClock::localTimezone() const {
    // The PS2 BIOS clock has no timezone database; report UTC. A later port can
    // derive the real offset from the system configuration if a JSR needs it.
    return "GMT+00:00";
}

} // namespace hal
} // namespace ps2
