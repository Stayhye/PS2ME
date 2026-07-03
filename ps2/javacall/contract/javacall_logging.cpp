// PS2 JavaCall port — contract layer (ABI boundary).
//
// These are the extern "C" javacall_* symbols the phoneME VM links against for
// console output. Compiled as C++ so they can call the HAL directly; the C linkage
// comes from the javacall_logging.h declarations (which are wrapped in extern "C").
// No logic lives here beyond marshalling — everything delegates to hal::Logger.
#include "javacall_logging.h"   // phoneME contract header (provided via -I at build)

#include "../hal/Logger.hpp"

extern "C" {

void javacall_print(const char* s) {
    ps2::hal::Logger::instance().print(s);
}

void javacall_print_chars(const char* s, int length) {
    ps2::hal::Logger::instance().printChars(s, length);
}

// printf length modifier for the 64-bit types on the ps2sdk gcc toolchain
// (jlong/julong are `long long`), matching the "%lld"/"%llu" family.
const char* javacall_jlong_format_specifier() {
    return "%lld";
}

const char* javacall_julong_format_specifier() {
    return "%llu";
}

} // extern "C"
