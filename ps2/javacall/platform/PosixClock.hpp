// PS2 JavaCall port — platform layer.
//
// PosixClock: an IClock backed by the portable C library (gettimeofday for both
// wall and monotonic time, nanosleep for sleep). ps2sdk's newlib provides these
// (gettimeofday via ee/libcglue), so this single backend serves both the host
// validation build and the PS2 target. A dedicated EE-RTC clock can replace it
// later without touching the HAL if higher-resolution wall time is needed.
#ifndef PS2_JAVACALL_PLATFORM_POSIXCLOCK_HPP
#define PS2_JAVACALL_PLATFORM_POSIXCLOCK_HPP

#include "../hal/IClock.hpp"

namespace ps2 {
namespace platform {

class PosixClock : public hal::IClock {
public:
    virtual javacall_int64 wallClockMillis();
    virtual javacall_int64 monotonicCounter();
    virtual javacall_int64 monotonicFrequency();
    virtual void           sleep(javacall_uint64 ms);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_POSIXCLOCK_HPP
