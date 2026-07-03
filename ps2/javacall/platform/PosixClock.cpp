// PS2 JavaCall port — platform layer. PosixClock implementation + registration.
#include "PosixClock.hpp"
#include "../hal/SystemClock.hpp"

#include <sys/time.h>   // gettimeofday, struct timeval
#include <time.h>       // nanosleep, struct timespec

namespace ps2 {
namespace platform {

namespace {
// Microseconds since the Unix epoch, as read from the system clock. Shared by
// the wall clock and the (best-effort) monotonic counter.
javacall_int64 nowMicros() {
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (javacall_int64)tv.tv_sec * 1000000 + (javacall_int64)tv.tv_usec;
}
} // namespace

javacall_int64 PosixClock::wallClockMillis() {
    return nowMicros() / 1000;
}

javacall_int64 PosixClock::monotonicCounter() {
    // gettimeofday is not guaranteed monotonic, but on the PS2 the wall clock is
    // never stepped, so it is monotonic in practice. The microsecond value below
    // is paired with a 1 MHz frequency. (A future EE cycle-counter clock can
    // replace this for true monotonicity.)
    return nowMicros();
}

javacall_int64 PosixClock::monotonicFrequency() {
    return 1000000;   // monotonicCounter() is in microseconds
}

void PosixClock::sleep(javacall_uint64 ms) {
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000);
    nanosleep(&req, 0);
}

namespace {
// Register the default clock at program startup, before the VM reads any time.
struct PosixClockRegistrar {
    PosixClock clock;
    PosixClockRegistrar() { hal::SystemClock::instance().setClock(&clock); }
};
PosixClockRegistrar g_posixClockRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
