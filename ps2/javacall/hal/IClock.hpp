// PS2 JavaCall port — HAL layer.
//
// IClock: the time-source abstraction behind SystemClock. It exposes two
// distinct clocks the VM needs — a wall clock (calendar time, for
// System.currentTimeMillis and timezone) and a monotonic counter (never runs
// backwards, for elapsed-time and profiling) — plus the platform sleep. The
// platform layer supplies a concrete source (EE timers / gettimeofday on PS2,
// the host libc under a test) without SystemClock knowing about hardware.
#ifndef PS2_JAVACALL_HAL_ICLOCK_HPP
#define PS2_JAVACALL_HAL_ICLOCK_HPP

#include <javacall_defs.h>   // javacall_int64 / javacall_uint64 (portable typedefs)

namespace ps2 {
namespace hal {

class IClock {
public:
    virtual ~IClock() {}

    /// Milliseconds since the Unix epoch (00:00:00 UTC, 1 Jan 1970). May jump if
    /// the system clock is adjusted — use the monotonic counter for durations.
    virtual javacall_int64 wallClockMillis() = 0;

    /// A free-running counter that never decreases. Its unit is defined by
    /// monotonicFrequency(); pair the two to convert to seconds/milliseconds.
    virtual javacall_int64 monotonicCounter() = 0;

    /// Ticks per second of monotonicCounter(). Must be > 0.
    virtual javacall_int64 monotonicFrequency() = 0;

    /// Block the calling context for at least @p ms milliseconds.
    virtual void sleep(javacall_uint64 ms) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_ICLOCK_HPP
