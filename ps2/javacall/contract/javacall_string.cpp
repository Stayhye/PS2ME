// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_towlower / javacall_towupper: in-place Unicode case folding
// used by the share/utils case-insensitive string helpers (javautil_wcsnicmp) and
// by MIDP. This is pure computation with no platform dependency, so it lives here
// directly rather than behind a HAL class. Folding covers ASCII and the Latin-1
// supplement (the ranges that matter for MIDP property/URL/class-name comparisons);
// other scripts pass through unchanged.
#include "javacall_string.h"   // phoneme contract header (provided via -I at build)

namespace {

javacall_utf16 toLower(javacall_utf16 c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<javacall_utf16>(c + 0x20);
    }
    // Latin-1 supplement: À–Þ -> à–þ, excluding × (0xD7).
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) {
        return static_cast<javacall_utf16>(c + 0x20);
    }
    return c;
}

javacall_utf16 toUpper(javacall_utf16 c) {
    if (c >= 'a' && c <= 'z') {
        return static_cast<javacall_utf16>(c - 0x20);
    }
    // Latin-1 supplement: à–þ -> À–Þ, excluding ÷ (0xF7).
    if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) {
        return static_cast<javacall_utf16>(c - 0x20);
    }
    return c;
}

} // namespace

extern "C" {

javacall_result javacall_towlower(javacall_utf16* chars, unsigned int count) {
    if (chars == 0) {
        return JAVACALL_FAIL;
    }
    for (unsigned int i = 0; i < count; ++i) {
        chars[i] = toLower(chars[i]);
    }
    return JAVACALL_OK;
}

javacall_result javacall_towupper(javacall_utf16* chars, unsigned int count) {
    if (chars == 0) {
        return JAVACALL_FAIL;
    }
    for (unsigned int i = 0; i < count; ++i) {
        chars[i] = toUpper(chars[i]);
    }
    return JAVACALL_OK;
}

} // extern "C"
