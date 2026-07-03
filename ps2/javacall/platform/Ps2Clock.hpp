// PS2 JavaCall port — platform layer.
//
// Ps2Clock: an IClock backed by the EE hardware timer (GetTimerSystemTime), the
// PS2 target's real clock. It replaces PosixClock because ps2sdk's gettimeofday
// does not advance on the EE (no RTC is wired through newlib): a frozen clock
// froze every Java timed-wait and our own event_receive timeout, leaving the AMS
// stuck on its 2-second splash timer. GetTimerSystemTime is a free-running 64-bit
// bus-clock counter that advances reliably on hardware and under PCSX2.
#ifndef PS2_JAVACALL_PLATFORM_PS2CLOCK_HPP
#define PS2_JAVACALL_PLATFORM_PS2CLOCK_HPP

#include "../hal/IClock.hpp"

namespace ps2 {
namespace platform {

class Ps2Clock : public hal::IClock {
public:
    virtual javacall_int64 wallClockMillis();
    virtual javacall_int64 monotonicCounter();
    virtual javacall_int64 monotonicFrequency();
    virtual void           sleep(javacall_uint64 ms);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2CLOCK_HPP
