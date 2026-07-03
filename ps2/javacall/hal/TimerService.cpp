// PS2 JavaCall port — HAL layer. TimerService implementation.
#include "TimerService.hpp"
#include "ITimerBackend.hpp"

namespace ps2 {
namespace hal {

TimerService& TimerService::instance() {
    static TimerService inst;
    return inst;
}

Timer* TimerService::allocate() {
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (!pool_[i].inUse()) {
            return &pool_[i];
        }
    }
    return 0;
}

Timer* TimerService::fromHandle(javacall_handle handle) {
    // Validate the handle points into our pool and is live, so a stale or bogus
    // handle can't scribble outside the array.
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (pool_[i].handle() == handle && pool_[i].inUse()) {
            return &pool_[i];
        }
    }
    return 0;
}

javacall_result TimerService::initialize(int intervalMs, bool cyclic,
                                         javacall_callback_func func,
                                         javacall_handle* out) {
    if (out == 0 || func == 0) {
        return JAVACALL_INVALID_ARGUMENT;
    }
    *out = 0;

    // A negative interval means "ignore the call" per the javacall_time contract.
    if (intervalMs < 0) {
        return JAVACALL_OK;
    }
    if (backend_ == 0) {
        return JAVACALL_FAIL;
    }

    Timer* timer = allocate();
    if (timer == 0) {
        return JAVACALL_FAIL;   // pool exhausted
    }

    timer->configure(intervalMs, cyclic, func);
    if (!backend_->arm(timer)) {
        timer->release();
        return JAVACALL_FAIL;
    }

    *out = timer->handle();
    return JAVACALL_OK;
}

javacall_result TimerService::finalize(javacall_handle handle) {
    Timer* timer = fromHandle(handle);
    if (timer == 0) {
        return JAVACALL_INVALID_ARGUMENT;
    }
    if (backend_ != 0) {
        backend_->disarm(timer);
    }
    timer->release();
    return JAVACALL_OK;
}

void TimerService::suspend(javacall_handle handle) {
    Timer* timer = fromHandle(handle);
    if (timer != 0) {
        timer->suspend();
    }
}

void TimerService::resume(javacall_handle handle) {
    Timer* timer = fromHandle(handle);
    if (timer != 0) {
        timer->resume();
    }
}

} // namespace hal
} // namespace ps2
