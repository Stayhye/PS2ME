// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_event(s)_* symbols: the CLDC event pipe the MIDP AMS loop
// (JavaTask) drains and the platform feeds. Thin shims over hal::EventQueue.
#include "javacall_events.h"   // phoneME contract header (provided via -I at build)

#include "../hal/EventQueue.hpp"

#ifdef PS2ME_PROFILE_FRAME
// Frame profiler (PASSO A-decomp): attribute event-wait time to (poll/idle). Defined in the
// platform layer (Ps2Framebuffer.cpp); resolved at the final link.
extern "C" long long ps2me_prof_now(void);
extern "C" void ps2me_prof_poll_add(long long startTick, int which);  // which: 0=event_receive 1=Thread.sleep
extern "C" void ps2me_prof_evreq_add(long timeoutMs);  // requested wait per event_receive call (diagnostic)
#endif

extern "C" {

javacall_result javacall_events_init(void) {
    return JAVACALL_OK;   // the queue is a static object, ready at startup
}

javacall_result javacall_events_finalize(void) {
    return JAVACALL_OK;
}

javacall_result javacall_event_receive(long timeToWaitInMillisec,
                                       /*OUT*/ unsigned char* binaryBuffer,
                                       int binaryBufferMaxLen,
                                       /*OUT*/ int* outEventLen) {
#ifdef PS2ME_PROFILE_FRAME
    ps2me_prof_evreq_add(timeToWaitInMillisec);   // how long the VM ASKED to wait (vs actual)
    const long long _pt = ps2me_prof_now();
    const javacall_result r = ps2::hal::EventQueue::instance().receive(
        timeToWaitInMillisec, binaryBuffer, binaryBufferMaxLen, outEventLen);
    ps2me_prof_poll_add(_pt, 0);   // 0 = event_receive
    return r;
#else
    return ps2::hal::EventQueue::instance().receive(
        timeToWaitInMillisec, binaryBuffer, binaryBufferMaxLen, outEventLen);
#endif
}

javacall_result javacall_event_send(unsigned char* binaryBuffer, int binaryBufferLen) {
    return ps2::hal::EventQueue::instance().send(binaryBuffer, binaryBufferLen);
}

} // extern "C"
