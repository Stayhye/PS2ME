// PS2 JavaCall port — platform layer. SifLock implementation.
#include "SifLock.hpp"

extern "C" {
#include <kernel.h>   // CreateSema / WaitSema / SignalSema / GetThreadId
}

namespace ps2 {
namespace platform {

namespace {
int g_sema  = -1;   // underlying binary semaphore (-1 until init())
int g_owner = 0;    // thread id currently holding the lock (0 = free)
int g_depth = 0;    // recursion depth for the owning thread
} // namespace

namespace SifLock {

void init() {
    if (g_sema >= 0) {
        return;
    }
    ee_sema_t s;
    s.count        = 1;
    s.max_count    = 1;
    s.init_count   = 1;
    s.wait_threads = 0;
    s.attr         = 0;
    s.option       = 0;
    g_sema = CreateSema(&s);
}

void acquire() {
    if (g_sema < 0) {
        return;   // pre-init: single-threaded boot, nothing to serialize
    }
    const int self = GetThreadId();
    if (g_owner == self) {   // recursive re-entry on the owning thread
        g_depth++;
        return;
    }
    WaitSema(g_sema);        // blocks until whoever holds it releases
    g_owner = self;
    g_depth = 1;
}

void release() {
    if (g_sema < 0) {
        return;
    }
    if (g_owner != GetThreadId()) {
        return;   // not the owner (defensive): never signal someone else's lock
    }
    if (--g_depth == 0) {
        g_owner = 0;
        SignalSema(g_sema);
    }
}

} // namespace SifLock

} // namespace platform
} // namespace ps2
