// PS2 JavaCall port — platform layer. StdoutSink implementation + registration.
#include "StdoutSink.hpp"
#include "../hal/Logger.hpp"

#include <cstdio>

namespace ps2 {
namespace platform {

void StdoutSink::write(const char* data, int length) {
    // ps2sdk's newlib routes stdout to the EE console (SIF/tty), which the
    // emulator surfaces in its log. fwrite handles embedded NULs and non-
    // terminated buffers correctly.
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
