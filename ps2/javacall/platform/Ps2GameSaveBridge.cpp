// PS2 JavaCall port — platform layer. C-ABI bridge for the game-save feature.
//
// The game-save snapshot runs on the MIDP/VM side (javaTask.c, C) where the rmfs lives, and
// the restore runs from the GameLoader shim's KNI. Both need to reach the memory-card
// service (Ps2MemCard) and the game list (GameStorage) -- C++ singletons in this javacall
// library. This file exposes the few operations they need behind a plain C ABI, so the C /
// KNI callers can link against them without pulling in the C++ platform headers.
//
// Each game that has save data gets its OWN top-level memory-card save "/PS2ME_<game>",
// keyed by the (stable) JAR name, holding "rms.dat" (the packed RecordStores) plus the
// icon.sys / icon.icn that make the console's OSD browser show it as a real save with the
// game's own icon and name (the icon is installed native-side; see Ps2MidpMain). This is a
// separate save from the launcher's own "/PS2ME" (favourites / settings / resolutions), so
// the two never collide. Legacy blobs written by the earlier scheme (a plain file
// "/PS2ME/rms_<game>.dat") are still read on restore, so upgrading keeps existing saves.
#include "Ps2MemCard.hpp"
#include "../hal/GameStorage.hpp"

#include <string.h>   // strlen
#include <ctype.h>    // tolower
#include <stdio.h>    // snprintf

namespace {

// Sanitized stem for a game = its JAR name minus a trailing ".jar", reduced to [A-Za-z0-9_]
// and capped, so it is a stable per-game key and a legal memory-card path component. Writes
// up to 20 chars + NUL into @p stem (min capacity 21). Returns the length, or -1 on error.
int gameStem(int index, char* stem, int cap) {
    const char* nm = ps2::hal::GameStorage::instance().nameAt(index);
    int n, i, s;

    if (nm == 0 || stem == 0 || cap < 21) {
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
    for (i = 0; i < n && s < 20; i++) {
        const char c = nm[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            stem[s++] = c;
        }
    }
    stem[s] = '\0';
    return s;
}

// The game's memory-card save directory "/PS2ME_<stem>" into @p out. Returns the length, or
// -1 on error. "PS2ME_" (6) + stem (<=20) = 26 chars, within the card's ~32-char name limit.
int gameDir(int index, char* out, int cap) {
    char stem[21];
    int s = gameStem(index, stem, (int)sizeof(stem));
    int w;
    if (out == 0 || cap < 16) {
        return -1;
    }
    if (s <= 0) {
        w = snprintf(out, cap, "/PS2ME_g%d", index);   // no usable chars: key by index
    } else {
        w = snprintf(out, cap, "/PS2ME_%s", stem);
    }
    return (w > 0 && w < cap) ? w : -1;
}

} // namespace

extern "C" {

// Whether a usable memory card is present (0/1).
int ps2mc_available(void) {
    return ps2::platform::Ps2MemCard::instance().available() ? 1 : 0;
}

// The game's save directory "/PS2ME_<stem>" into @p out (native side installs the icon
// there). Returns strlen(out), or -1 on error.
int ps2gs_save_dir(int index, char* out, int cap) {
    return gameDir(index, out, cap);
}

// Write @p len bytes from @p blob as the game's "/PS2ME_<stem>/rms.dat", creating the
// per-game save directory first. Returns 0 on success, -1 on failure. Snapshot calls this.
int ps2gs_write_save(int index, const void* blob, int len) {
    char dir[64], path[80];
    if (gameDir(index, dir, (int)sizeof(dir)) <= 0) {
        return -1;
    }
    if (!ps2::platform::Ps2MemCard::instance().ensureDir(dir)) {
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/rms.dat", dir) <= 0) {
        return -1;
    }
    return ps2::platform::Ps2MemCard::instance().writeFile(path, blob, len) ? 0 : -1;
}

// Read the game's save into @p buf (capacity @p cap). Prefers the new
// "/PS2ME_<stem>/rms.dat"; if that is absent, falls back to a legacy "/PS2ME/rms_<stem>.dat"
// blob so saves written before the per-game-directory scheme are not lost (the next snapshot
// rewrites them to the new location). Returns bytes read, or <= 0 when absent. Restore calls
// this.
int ps2gs_read_save(int index, void* buf, int cap) {
    char dir[64], path[80];
    int got;
    if (gameDir(index, dir, (int)sizeof(dir)) <= 0) {
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/rms.dat", dir) <= 0) {
        return -1;
    }
    got = ps2::platform::Ps2MemCard::instance().readFile(path, buf, cap);
    if (got > 0) {
        return got;
    }
    // Legacy fallback: "/PS2ME/rms_<stem>.dat" (via the config path, which prefixes /PS2ME).
    {
        char stem[21], legacy[40];
        int s = gameStem(index, stem, (int)sizeof(stem));
        if (s > 0) {
            snprintf(legacy, sizeof(legacy), "rms_%s.dat", stem);
        } else {
            snprintf(legacy, sizeof(legacy), "rms_g%d.dat", index);
        }
        return ps2::platform::Ps2MemCard::instance().configRead(legacy, buf, cap);
    }
}

} // extern "C"
