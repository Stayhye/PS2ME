// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_os_* symbols the phoneME VM's Os porting layer
// (cldc/src/vm/os/javacall/OS_javacall.cpp) links against: one-time OS bring-up
// and instruction-cache maintenance. Thin shims — all behaviour lives in
// hal::OsCore.
//
// The mutex/condition-variable functions declared in javacall_os.h are only
// needed by a native-threaded (MIDP) build; the CLDC VM uses green threads on a
// single OS thread, so they are intentionally omitted for Milestone A (the
// reference win32_x86_cldc port omits them too).
#include "javacall_os.h"   // phoneME contract header (provided via -I at build)

#include "../hal/OsCore.hpp"

extern "C" {

void javacall_os_initialize(void) {
    ps2::hal::OsCore::instance().initialize();
}

void javacall_os_dispose() {
    ps2::hal::OsCore::instance().dispose();
}

void javacall_os_flush_icache(unsigned char* address, int size) {
    ps2::hal::OsCore::instance().flushInstructionCache(address, size);
}

} // extern "C"
