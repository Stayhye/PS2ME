// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_lifecycle_* symbols the VM calls to announce MIDlet state
// transitions and to request external URL handling. Thin shims over
// hal::MidletLifecycle. (The javanotify_* start/pause/resume direction and
// JavaTask itself are provided by the MIDP library, not here.)
#include "javacall_lifecycle.h"   // phoneME contract header (provided via -I at build)

#include "../hal/MidletLifecycle.hpp"

extern "C" {

void javacall_lifecycle_state_changed(javacall_lifecycle_state state,
                                      javacall_result status,
                                      struct javacall_lifecycle_additional_info* /*additionalInfo*/) {
    ps2::hal::MidletLifecycle::instance().notifyStateChanged(
        static_cast<int>(state), status);
}

javacall_result javacall_lifecycle_platform_request(char* urlString) {
    return ps2::hal::MidletLifecycle::instance().platformRequest(urlString);
}

} // extern "C"
