// PS2 JavaCall port — HAL layer.
//
// SystemClock: the time service the javacall_time contract's clock functions
// delegate to. It owns no hardware; it derives every value the VM asks for
// (calendar time, elapsed time, monotonic counter, sleep, timezone) from an
// injected IClock, keeping the arithmetic portable and testable.
#ifndef PS2_JAVACALL_HAL_SYSTEMCLOCK_HPP
#define PS2_JAVACALL_HAL_SYSTEMCLOCK_HPP

#include <javacall_defs.h>

namespace ps2 {
namespace hal {

class IClock;

class SystemClock {
public:
    /// Process-wide instance. Constructed on first use (safe static init order
    /// with the platform clock that registers itself at startup).
    static SystemClock& instance();

    /// Install the time source. Ownership stays with the caller (typically a
    /// static platform object). With no source installed the clock reads as 0.
    void setClock(IClock* clock) { clock_ = clock; }
    IClock* clock() const { return clock_; }

    /// Milliseconds since the Unix epoch — javacall_time_get_milliseconds_since_1970.
    javacall_int64 wallClockMillis();

    /// Whole seconds since the Unix epoch — javacall_time_get_seconds_since_1970.
    javacall_int64 wallClockSeconds();

    /// A monotonic millisecond counter for elapsed-time measurement —
    /// javacall_time_get_clock_milliseconds. Derived from the monotonic source
    /// so it never runs backwards even if the wall clock is adjusted.
    javacall_int64 elapsedMillis();

    /// Raw monotonic counter and its frequency — the SUPPORTS_MONOTONIC_CLOCK
    /// path used by Os::elapsed_counter()/elapsed_frequency().
    javacall_int64 monotonicCounter();
    javacall_int64 monotonicFrequency();

    /// Block the caller for at least @p ms milliseconds — javacall_time_sleep.
    void sleep(javacall_uint64 ms);

    /// Local timezone in "GMT+/-HH:MM" form — javacall_time_get_local_timezone.
    /// The returned buffer is owned by SystemClock; the caller must not free it.
    const char* localTimezone() const;

private:
    SystemClock() : clock_(0) {}
    SystemClock(const SystemClock&);            // non-copyable
    SystemClock& operator=(const SystemClock&);

    IClock* clock_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_SYSTEMCLOCK_HPP
