// PS2 JavaCall port — platform layer.
//
// NullEventLock: the event-queue lock backend for the single-OS-thread CLDC/MIDP
// build. The VM multiplexes green threads onto one native thread, so event-queue
// access is never concurrent and the lock reduces to a no-op. It still exists as a
// real object so the locking contract is honoured and a preemptive backend can
// replace it without touching EventQueue.
#ifndef PS2_JAVACALL_PLATFORM_NULLEVENTLOCK_HPP
#define PS2_JAVACALL_PLATFORM_NULLEVENTLOCK_HPP

#include "../hal/IEventLock.hpp"

namespace ps2 {
namespace platform {

class NullEventLock : public hal::IEventLock {
public:
    virtual void lock() {}
    virtual void unlock() {}
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_NULLEVENTLOCK_HPP
