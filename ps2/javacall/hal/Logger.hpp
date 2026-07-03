// PS2 JavaCall port — HAL layer.
//
// Logger: the console/logging service. The VM's stdout (System.out) and internal
// tracing arrive here via the javacall_logging contract shims. The Logger owns no
// hardware; it forwards bytes to an injected IOutputSink, so the same logic runs
// on real PS2 hardware or under a host test.
#ifndef PS2_JAVACALL_HAL_LOGGER_HPP
#define PS2_JAVACALL_HAL_LOGGER_HPP

namespace ps2 {
namespace hal {

class IOutputSink;

class Logger {
public:
    /// Process-wide instance. Constructed on first use (safe static init order).
    static Logger& instance();

    /// Install the sink that receives all output. Ownership stays with the caller
    /// (typically a static platform object). Passing nullptr silences output.
    void setSink(IOutputSink* sink) { sink_ = sink; }
    IOutputSink* sink() const { return sink_; }

    /// Print a NUL-terminated string.
    void print(const char* s);

    /// Print @p length bytes (not necessarily NUL-terminated).
    void printChars(const char* s, int length);

private:
    Logger() : sink_(0) {}
    Logger(const Logger&);            // non-copyable
    Logger& operator=(const Logger&);

    IOutputSink* sink_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_LOGGER_HPP
