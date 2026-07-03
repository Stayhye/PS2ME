// PS2 JavaCall port — HAL layer. Storage implementation.
#include "Storage.hpp"

namespace ps2 {
namespace hal {

namespace {
// Logical MIDP storage locations. Kept as plain paths; the leading component
// maps to a PS2 device (e.g. a memory card) once real fileio lands.
const char ROOT_PATH[]   = "/j2me/appdb/";
const char CONFIG_PATH[] = "/j2me/lib/";
} // namespace

Storage& Storage::instance() {
    static Storage inst;
    return inst;
}

javacall_result Storage::fill(const char* ascii, javacall_utf16* out, int* ioLen) {
    if (out == 0 || ioLen == 0) {
        return JAVACALL_FAIL;
    }
    const int maxLen = *ioLen;
    int i = 0;
    for (; ascii[i] != '\0'; ++i) {
        if (i >= maxLen) {
            return JAVACALL_FAIL;   // does not fit the caller's buffer
        }
        out[i] = static_cast<javacall_utf16>(ascii[i]);
    }
    *ioLen = i;
    return JAVACALL_OK;
}

javacall_result Storage::rootPath(javacall_utf16* out, int* ioLen) {
    return fill(ROOT_PATH, out, ioLen);
}

javacall_result Storage::configPath(javacall_utf16* out, int* ioLen) {
    return fill(CONFIG_PATH, out, ioLen);
}

} // namespace hal
} // namespace ps2
