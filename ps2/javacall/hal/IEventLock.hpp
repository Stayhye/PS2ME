// PS2 JavaCall port — HAL layer.
//
// IEventLock: the mutual-exclusion primitive guarding the event queue. The MIDP
// event system (javacall_eventqueue.h) brackets its queue access with
// wait_and_lock / unlock. Abstracting it keeps EventQueue free of any concurrency
// backend: the CLDC/MIDP build runs green threads on a single OS thread, so the
// PS2 backend is a no-op, but a future preemptive backend can drop in here.
#ifndef PS2_JAVACALL_HAL_IEVENTLOCK_HPP
#define PS2_JAVACALL_HAL_IEVENTLOCK_HPP

namespace ps2 {
namespace hal {

class IEventLock {
public:
    virtual ~IEventLock() {}
    virtual void lock() = 0;
    virtual void unlock() = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IEVENTLOCK_HPP
