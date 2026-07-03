// PS2 JavaCall port — platform layer.
//
// StdoutSink: writes to the Emotion Engine's standard output (visible in the
// emulator/console log). This is the bring-up console for Milestone A. A later
// on-screen console sink (libgraph/libdraw) can replace or supplement it without
// touching the HAL or contract layers.
#ifndef PS2_JAVACALL_PLATFORM_STDOUTSINK_HPP
#define PS2_JAVACALL_PLATFORM_STDOUTSINK_HPP

#include "../hal/IOutputSink.hpp"

namespace ps2 {
namespace platform {

class StdoutSink : public hal::IOutputSink {
public:
    virtual void write(const char* data, int length);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_STDOUTSINK_HPP
