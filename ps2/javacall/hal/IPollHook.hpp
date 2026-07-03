// PS2 JavaCall port — HAL layer.
//
// IPollHook: a callback the EventQueue invokes on every iteration of its blocking
// receive() loop. It lets the platform feed asynchronous input (the controller)
// into the event pipe while the VM waits, without the EventQueue knowing anything
// about input devices. Keypad implements this; nothing else in the queue changes.
#ifndef PS2_JAVACALL_HAL_IPOLLHOOK_HPP
#define PS2_JAVACALL_HAL_IPOLLHOOK_HPP

namespace ps2 {
namespace hal {

class IPollHook {
public:
    virtual ~IPollHook() {}

    /// Called by EventQueue::receive() each poll cycle (and once in poll-only
    /// mode). Implementations should be cheap and non-blocking.
    virtual void onPoll() = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IPOLLHOOK_HPP
