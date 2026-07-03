// PS2 JavaCall port — platform layer.
//
// PosixTickTimer: an ITimerBackend for the host validation build, implemented
// with setitimer(ITIMER_REAL) + SIGALRM. It drives a single active timer, which
// is exactly what the CLDC VM needs (the one cyclic scheduler tick), so it lets
// the whole javacall_time module be exercised on the host. The PS2 target uses
// Ps2AlarmTimer instead; only one backend is compiled per build.
#ifndef PS2_JAVACALL_PLATFORM_POSIXTICKTIMER_HPP
#define PS2_JAVACALL_PLATFORM_POSIXTICKTIMER_HPP

#include "../hal/ITimerBackend.hpp"

namespace ps2 {
namespace platform {

class PosixTickTimer : public hal::ITimerBackend {
public:
    virtual bool arm(hal::Timer* timer);
    virtual void disarm(hal::Timer* timer);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_POSIXTICKTIMER_HPP
