// PS2 JavaCall port — HAL layer. Logger implementation.
#include "Logger.hpp"
#include "IOutputSink.hpp"

#include <cstring>

namespace ps2 {
namespace hal {

Logger& Logger::instance() {
    // Function-local static: constructed on first call, no static-init-order issue
    // with the platform sink that registers itself at program startup.
    static Logger inst;
    return inst;
}

void Logger::printChars(const char* s, int length) {
    if (sink_ != 0 && s != 0 && length > 0) {
        sink_->write(s, length);
    }
}

void Logger::print(const char* s) {
    if (s != 0) {
        printChars(s, static_cast<int>(std::strlen(s)));
    }
}

} // namespace hal
} // namespace ps2
