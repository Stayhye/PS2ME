// PS2 JavaCall port — platform layer. C-ABI bridge for the game-save feature.
//
// The game-save snapshot runs on the MIDP/VM side (javaTask.c, C) where the rmfs lives, and
// the restore will run from the GameLoader shim's KNI. Both need to reach the memory-card
// service (Ps2MemCard) and the game list (GameStorage) -- C++ singletons in this javacall
// library. This file exposes the few operations they need behind a plain C ABI, so the C /
// KNI callers can link against them without pulling in the C++ platform headers. The save
// blob itself lives under the launcher's "/PS2ME" directory as "rms_<game>.dat", reusing
// the proven config read/write path (a per-game card save with its own browser icon is a
// later polish, not needed for the progress to persist).
#include "Ps2MemCard.hpp"
#include "../hal/GameStorage.hpp"

#include <string.h>   // strlen
#include <ctype.h>    // tolower
#include <stdio.h>    // snprintf

extern "C" {

// Whether a usable memory card is present (0/1).
int ps2mc_available(void) {
    return ps2::platform::Ps2MemCard::instance().available() ? 1 : 0;
}

// Write @p len bytes from @p data as launcher-config file @p name ("/PS2ME/<name>").
// Returns 0 on success, -1 on failure.
int ps2mc_config_write(const char* name, const void* data, int len) {
    return ps2::platform::Ps2MemCard::instance().configWrite(name, data, len) ? 0 : -1;
}

// Read launcher-config file @p name into @p buf (capacity @p cap). Returns bytes read,
// or <= 0 when absent.
int ps2mc_config_read(const char* name, void* buf, int cap) {
    return ps2::platform::Ps2MemCard::instance().configRead(name, buf, cap);
}

// Build the memory-card save filename for game @p index into @p out: "rms_<stem>.dat",
// where <stem> is the game's JAR name minus a trailing ".jar", sanitized to [A-Za-z0-9_]
// and capped, so the save is keyed by the (stable) game identity and is a legal card name.
// Returns strlen(out), or -1 on error. Snapshot and restore both call this so they agree.
int ps2gs_save_name(int index, char* out, int cap) {
    const char* nm = ps2::hal::GameStorage::instance().nameAt(index);
    char stem[21];
    int n, i, s, w;

    if (nm == 0 || out == 0 || cap < 12) {
        return -1;
    }
    n = (int)strlen(nm);
    if (n >= 4 && nm[n - 4] == '.' &&
        tolower((unsigned char)nm[n - 3]) == 'j' &&
        tolower((unsigned char)nm[n - 2]) == 'a' &&
        tolower((unsigned char)nm[n - 1]) == 'r') {
        n -= 4;                         // drop a trailing ".jar"
    }
    s = 0;
    for (i = 0; i < n && s < (int)sizeof(stem) - 1; i++) {
        const char c = nm[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            stem[s++] = c;
        }
    }
    stem[s] = '\0';
    if (s == 0) {
        // Name had no usable characters: key by index so it is still unique and legal.
        w = snprintf(out, cap, "rms_g%d.dat", index);
    } else {
        w = snprintf(out, cap, "rms_%s.dat", stem);
    }
    return (w > 0 && w < cap) ? w : -1;
}

} // extern "C"
