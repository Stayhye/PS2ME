// PS2 JavaCall port — HAL layer. EventQueue implementation.
#include "EventQueue.hpp"
#include "IEventLock.hpp"
#include "IPollHook.hpp"
#include "SystemClock.hpp"

#include <cstring>   // std::memcpy

namespace ps2 {
namespace hal {

namespace {
// Poll granularity while blocking in receive(). Small enough to stay responsive,
// large enough not to spin. The scheduler tick is 30 ms, so 5 ms is comfortable.
const javacall_uint64 POLL_INTERVAL_MS = 5;

class ScopedLock {
public:
    explicit ScopedLock(IEventLock* l) : lock_(l) { if (lock_) lock_->lock(); }
    ~ScopedLock() { if (lock_) lock_->unlock(); }
private:
    IEventLock* lock_;
    ScopedLock(const ScopedLock&);
    ScopedLock& operator=(const ScopedLock&);
};
} // namespace

EventQueue& EventQueue::instance() {
    static EventQueue inst;
    return inst;
}

javacall_result EventQueue::send(const unsigned char* buffer, int len) {
    if (buffer == 0 || len <= 0 || len > MAX_EVENT_BYTES) {
        return JAVACALL_FAIL;
    }
    ScopedLock guard(lock_);
    if (count_ >= MAX_EVENTS) {
        return JAVACALL_FAIL;       // queue full: drop rather than overwrite
    }
    const int tail = (head_ + count_) % MAX_EVENTS;
    std::memcpy(slots_[tail].data, buffer, static_cast<size_t>(len));
    slots_[tail].len = len;
    ++count_;
    return JAVACALL_OK;
}

int EventQueue::pop(unsigned char* buffer, int maxLen, bool* overflow) {
    if (count_ == 0) {
        return -1;
    }
    Slot& slot = slots_[head_];
    int copy = slot.len;
    *overflow = false;
    if (copy > maxLen) {
        copy = maxLen;
        *overflow = true;
    }
    std::memcpy(buffer, slot.data, static_cast<size_t>(copy));
    const int len = slot.len;
    head_ = (head_ + 1) % MAX_EVENTS;
    --count_;
    return *overflow ? len : copy;
}

javacall_result EventQueue::receive(long timeoutMs, unsigned char* buffer, int maxLen,
                                    int* outLen) {
    if (buffer == 0 || maxLen <= 0) {
        if (outLen) *outLen = 0;
        return JAVACALL_FAIL;
    }

    SystemClock& clock = SystemClock::instance();
    const javacall_int64 deadline =
        (timeoutMs < 0) ? 0 : clock.elapsedMillis() + timeoutMs;

    for (;;) {
        // Feed asynchronous input (controller) into the queue before draining it,
        // so key events raised here are seen on this very iteration. Runs outside
        // the lock: onPoll() may call send(), which takes the lock itself.
        if (pollHook_ != 0) {
            pollHook_->onPoll();
        }

        bool overflow = false;
        int  got;
        {
            ScopedLock guard(lock_);
            got = pop(buffer, maxLen, &overflow);
        }
        if (got >= 0) {
            if (outLen) *outLen = got;
            return overflow ? JAVACALL_OUT_OF_MEMORY : JAVACALL_OK;
        }

        // Queue empty. Honour the timeout contract: 0 = return immediately.
        if (timeoutMs == 0) {
            if (outLen) *outLen = 0;
            return JAVACALL_FAIL;
        }
        // Nap POLL_INTERVAL_MS, but for a bounded wait clamp the LAST nap to the time
        // still left before the deadline. Ps2Clock::sleep busy-waits a full quantum, so
        // without the clamp a bounded wait rounds UP to the next 5 ms multiple, waking the
        // VM's green thread up to ~5 ms LATE every call. That overshoot steals frame budget
        // (never gains -- we only stop waking late, never wake before the game asked).
        javacall_uint64 nap = POLL_INTERVAL_MS;
        if (timeoutMs > 0) {
            const javacall_int64 remaining = deadline - clock.elapsedMillis();
            if (remaining <= 0) {
                if (outLen) *outLen = 0;
                return JAVACALL_FAIL;
            }
            if ((javacall_int64)nap > remaining) {
                nap = (javacall_uint64)remaining;
            }
        }
        clock.sleep(nap);   // timeoutMs < 0 (forever) keeps the full quantum
    }
}

} // namespace hal
} // namespace ps2
