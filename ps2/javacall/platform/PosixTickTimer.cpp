// PS2 JavaCall port — platform layer. PosixTickTimer implementation + registration.
#include "PosixTickTimer.hpp"
#include "../hal/Timer.hpp"
#include "../hal/TimerService.hpp"

#include <signal.h>
#include <sys/time.h>   // setitimer, struct itimerval

namespace ps2 {
namespace platform {

namespace {

// This backend supports one active timer — enough for the scheduler tick and
// all the CLDC VM asks for. The SIGALRM handler needs to reach it statically.
hal::Timer* g_activeTimer = 0;

void setInterval(int intervalMs, bool repeat) {
    struct itimerval it;
    it.it_value.tv_sec     = intervalMs / 1000;
    it.it_value.tv_usec    = (intervalMs % 1000) * 1000;
    it.it_interval.tv_sec  = repeat ? it.it_value.tv_sec  : 0;
    it.it_interval.tv_usec = repeat ? it.it_value.tv_usec : 0;
    setitimer(ITIMER_REAL, &it, 0);
}

void disable() {
    struct itimerval it;
    it.it_value.tv_sec = it.it_value.tv_usec = 0;
    it.it_interval.tv_sec = it.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, 0);
}

void onSigalrm(int) {
    hal::Timer* timer = g_activeTimer;
    if (timer == 0) {
        return;
    }
    // The kernel already re-armed a cyclic timer via it_interval, so we only act
    // on onFire()'s verdict for the one-shot case: stop when it says not to repeat.
    if (!timer->onFire()) {
        disable();
        g_activeTimer = 0;
    }
}

void installHandler() {
    struct sigaction sa;
    sa.sa_handler = onSigalrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, 0);
}

} // namespace

bool PosixTickTimer::arm(hal::Timer* timer) {
    if (timer == 0 || g_activeTimer != 0) {
        return false;   // single-timer backend already busy
    }
    installHandler();
    g_activeTimer = timer;
    setInterval(timer->intervalMs(), timer->cyclic());
    return true;
}

void PosixTickTimer::disarm(hal::Timer* timer) {
    if (g_activeTimer == timer) {
        disable();
        g_activeTimer = 0;
    }
}

namespace {
// Register the host timer backend at program startup.
struct PosixTickTimerRegistrar {
    PosixTickTimer backend;
    PosixTickTimerRegistrar() { hal::TimerService::instance().setBackend(&backend); }
};
PosixTickTimerRegistrar g_posixTickTimerRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
