// PS2 JavaCall port — HAL layer.
//
// EventQueue: the binary event pipe between the platform and the VM. The MIDP
// AMS loop (JavaTask) drains it via javacall_event_receive; the platform feeds it
// via javacall_event_send (lifecycle start events now; key/pen input in later
// milestones). Events are opaque byte blobs (a midp_jc_event_union), stored in a
// fixed ring of max-size slots so no allocation happens on the event path.
//
// Blocking-receive semantics live here: with no preemptive threads, receive()
// polls the ring and sleeps on the shared SystemClock until an event lands or the
// caller's timeout expires.
#ifndef PS2_JAVACALL_HAL_EVENTQUEUE_HPP
#define PS2_JAVACALL_HAL_EVENTQUEUE_HPP

#include <javacall_defs.h>

namespace ps2 {
namespace hal {

class IEventLock;
class IPollHook;

class EventQueue {
public:
    /// Capacity in events and the per-event byte cap (a midp_jc_event_union fits
    /// well under JAVACALL_MAX_EVENT_SIZE).
    static const int MAX_EVENTS = 32;
    static const int MAX_EVENT_BYTES = JAVACALL_MAX_EVENT_SIZE;

    static EventQueue& instance();

    /// Install the lock backend. Ownership stays with the caller.
    void setLock(IEventLock* lock) { lock_ = lock; }

    /// Install a hook pumped on every receive() poll cycle (input feeding).
    /// Ownership stays with the caller.
    void setPollHook(IPollHook* hook) { pollHook_ = hook; }

    /// javacall_event_send: copy @p len bytes into the queue.
    /// @return JAVACALL_OK, or JAVACALL_FAIL if full or oversized.
    javacall_result send(const unsigned char* buffer, int len);

    /// javacall_event_receive: wait up to @p timeoutMs (0 = poll, <0 = forever)
    /// for an event and copy it into @p buffer.
    /// @return JAVACALL_OK on delivery, JAVACALL_FAIL on timeout/empty,
    ///         JAVACALL_OUT_OF_MEMORY if the event exceeds @p maxLen (the first
    ///         maxLen bytes are still copied and the event consumed).
    javacall_result receive(long timeoutMs, unsigned char* buffer, int maxLen,
                            int* outLen);

private:
    EventQueue() : lock_(0), pollHook_(0), head_(0), count_(0) {}
    EventQueue(const EventQueue&);
    EventQueue& operator=(const EventQueue&);

    // Non-blocking dequeue of the oldest event. Returns its length, or -1 if the
    // queue is empty. Caller holds the lock.
    int pop(unsigned char* buffer, int maxLen, bool* overflow);

    IEventLock* lock_;
    IPollHook*  pollHook_;
    struct Slot {
        unsigned char data[MAX_EVENT_BYTES];
        int           len;
    };
    Slot slots_[MAX_EVENTS];
    int  head_;    // index of the oldest event
    int  count_;   // number of queued events
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_EVENTQUEUE_HPP
