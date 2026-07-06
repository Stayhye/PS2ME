// PS2 JavaCall port — platform layer. StdoutSink implementation + registration.
#include "StdoutSink.hpp"
#include "../hal/Logger.hpp"
#include "Ps2Frontend.hpp"
#include "SifLock.hpp"

#include <cstdio>

namespace ps2 {
namespace platform {

void StdoutSink::write(const char* data, int length) {
    // Mirror to the native on-screen launch trace FIRST (a no-op unless the launcher
    // enabled it): on real hardware the SIF/EE console is gone after the USB IOP reset,
    // so this is the only place the VM's stdout (System.out incl. the [Launcher]
    // milestones and exception traces) is visible while a game installs and starts.
    // Doing it before the stdout write means a stalled console can't hide the trace.
    Ps2Frontend::instance().logWrite(data, length);

    // ps2sdk's newlib routes stdout to the EE console (SIF/tty), which the emulator
    // surfaces in its log. fwrite handles embedded NULs and non-terminated buffers
    // correctly. The write reaches the IOP over SIF, so serialize it against the audio
    // mixer thread and controller reads on the shared lock.
    SifGuard guard;
    std::fwrite(data, 1, static_cast<size_t>(length), stdout);
    std::fflush(stdout);
}

namespace {
// Register the default console sink at program startup, before the VM makes any
// javacall_print call. Kept in the platform layer so the HAL stays hardware-free.
struct DefaultSinkRegistrar {
    StdoutSink sink;
    DefaultSinkRegistrar() { hal::Logger::instance().setSink(&sink); }
};
DefaultSinkRegistrar g_defaultSinkRegistrar;
} // namespace

} // namespace platform
} // namespace ps2
