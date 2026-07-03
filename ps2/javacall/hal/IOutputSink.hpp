// PS2 JavaCall port — HAL layer.
//
// IOutputSink: the abstraction the console/logging service writes bytes to.
// Defining this interface here (instead of calling ps2sdk directly) is what keeps
// the HAL portable and testable: the platform layer supplies a concrete sink
// (screen console, EE stdout, a file, or a test double) without the HAL knowing
// anything about the hardware.
#ifndef PS2_JAVACALL_HAL_IOUTPUTSINK_HPP
#define PS2_JAVACALL_HAL_IOUTPUTSINK_HPP

namespace ps2 {
namespace hal {

class IOutputSink {
public:
    virtual ~IOutputSink() {}

    /// Write @p length bytes starting at @p data to the underlying stream.
    /// @p data is not required to be NUL-terminated.
    virtual void write(const char* data, int length) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IOUTPUTSINK_HPP
