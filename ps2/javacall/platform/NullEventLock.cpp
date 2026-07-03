// PS2 JavaCall port — platform layer. NullEventLock registration.
//
// Installs the no-op event-queue lock into hal::EventQueue at program startup,
// before the VM sends or receives any event. Mirrors the StdoutSink/Ps2AlarmTimer/
// Ps2Framebuffer registrars.
#include "NullEventLock.hpp"
#include "../hal/EventQueue.hpp"

namespace ps2 {
namespace platform {

namespace {
NullEventLock g_nullEventLock;

struct NullEventLockRegistrar {
    NullEventLockRegistrar() {
        hal::EventQueue::instance().setLock(&g_nullEventLock);
    }
};
NullEventLockRegistrar g_nullEventLockRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
