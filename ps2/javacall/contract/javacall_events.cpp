// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_event(s)_* symbols: the CLDC event pipe the MIDP AMS loop
// (JavaTask) drains and the platform feeds. Thin shims over hal::EventQueue.
#include "javacall_events.h"   // phoneME contract header (provided via -I at build)

#include "../hal/EventQueue.hpp"

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
    return ps2::hal::EventQueue::instance().receive(
        timeToWaitInMillisec, binaryBuffer, binaryBufferMaxLen, outEventLen);
}

javacall_result javacall_event_send(unsigned char* binaryBuffer, int binaryBufferLen) {
    return ps2::hal::EventQueue::instance().send(binaryBuffer, binaryBufferLen);
}

} // extern "C"
