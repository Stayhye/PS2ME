// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_*_event_queue_lock symbols: the mutex the MIDP native event
// system uses to guard its own message queue. On the single-OS-thread CLDC/MIDP
// build there is no concurrent access, so the lock is a no-op that always reports
// success — a NOT_IMPLEMENTED here would read as a locking failure to MIDP.
#include "javacall_eventqueue.h"   // phoneME contract header (provided via -I at build)

extern "C" {

javacall_result javacall_create_event_queue_lock(void) {
    return JAVACALL_OK;
}

javacall_result javacall_destroy_event_queue_lock(void) {
    return JAVACALL_OK;
}

javacall_result javacall_wait_and_lock_event_queue(void) {
    return JAVACALL_OK;
}

javacall_result javacall_unlock_event_queue(void) {
    return JAVACALL_OK;
}

} // extern "C"
