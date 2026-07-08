// PS2 JavaCall port — platform layer. Resolutions implementation.
//
// Per-game canvas size, stored by JAR name (one "name=WxH" line) in
// <bootdir>resolutions.txt, resolved to game indices at load(). See Resolutions.hpp.
#include "Resolutions.hpp"

#include "Ps2Storage.hpp"
#include "SifLock.hpp"
#include "../hal/GameStorage.hpp"

#include <fcntl.h>      // open, O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC
#include <unistd.h>     // read, write, close
#include <stdio.h>      // snprintf
#include <string.h>     // strcmp, strlen

namespace ps2 {
namespace platform {

namespace {
// Selectable presets. Index 0 is the default (portrait QVGA). Each dimension is <= 512 so
// the raster fits the GS game texture and the max backing store (see Ps2Framebuffer).
// Common J2ME device screens, portrait and landscape.
struct Preset { int w, h; const char* label; };
const Preset kPresets[] = {
    { 240, 320, "240x320" },   // 0: QVGA portrait (default)
    { 320, 240, "320x240" },   // 1: QVGA landscape
    { 176, 208, "176x208" },   // 2: Series 60 portrait
    { 208, 176, "208x176" },   // 3: Series 60 landscape
    { 176, 220, "176x220" },   // 4: Series 40 portrait
    { 220, 176, "220x176" },   // 5: Series 40 landscape
    { 128, 160, "128x160" },   // 6
    { 128, 128, "128x128" },   // 7
    { 240, 400, "240x400" },   // 8: tall portrait
    { 400, 240, "400x240" },   // 9: wide landscape
    { 352, 416, "352x416" },   // 10
};
const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

// Find the preset index whose size is exactly (w,h); -1 if none.
int presetForSize(int w, int h) {
    for (int i = 0; i < kPresetCount; ++i) {
        if (kPresets[i].w == w && kPresets[i].h == h) {
            return i;
        }
    }
    return -1;
}

// Scan @p name for the first "<digits>x<digits>" token and return the matching preset
// index, or -1 if none is embedded / it isn't a known preset. Lets filenames like
// "3d_rollercoaster_rush_240x320-88383.jar" auto-pick their size.
int presetFromName(const char* name) {
    if (name == 0) {
        return -1;
    }
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            continue;
        }
        int w = 0, digits = 0;
        const char* q = p;
        while (*q >= '0' && *q <= '9' && digits < 5) { w = w * 10 + (*q - '0'); ++q; ++digits; }
        if ((*q != 'x' && *q != 'X') || digits == 0) {
            continue;
        }
        ++q;
        int h = 0, hd = 0;
        while (*q >= '0' && *q <= '9' && hd < 5) { h = h * 10 + (*q - '0'); ++q; ++hd; }
        if (hd == 0) {
            continue;
        }
        const int idx = presetForSize(w, h);
        if (idx >= 0) {
            return idx;
        }
        p = q - 1;   // keep scanning past this token
    }
    return -1;
}
} // namespace

Resolutions& Resolutions::instance() {
    static Resolutions inst;
    return inst;
}

Resolutions::Resolutions() : loaded_(false), gameCount_(0) {
    for (int i = 0; i < MAX_GAMES; ++i) {
        idx_[i] = 0;
        def_[i] = 0;
    }
}

int Resolutions::presetCount() {
    return kPresetCount;
}

const char* Resolutions::preset(int i, int* w, int* h) {
    if (i < 0 || i >= kPresetCount) {
        i = 0;
    }
    if (w != 0) *w = kPresets[i].w;
    if (h != 0) *h = kPresets[i].h;
    return kPresets[i].label;
}

void Resolutions::load(int gameCount) {
    gameCount_ = gameCount < 0 ? 0 : (gameCount > MAX_GAMES ? MAX_GAMES : gameCount);
    loaded_ = true;

    // Defaults: auto-detect each game's size from its filename (else portrait QVGA).
    for (int g = 0; g < gameCount_; ++g) {
        const int d = presetFromName(hal::GameStorage::instance().nameAt(g));
        def_[g] = (unsigned char)(d >= 0 ? d : 0);
        idx_[g] = def_[g];
    }

    char path[256];
    if (!Ps2Storage::instance().resolutionsPath(path, (int)sizeof(path))) {
        return;
    }
    SifGuard guard;   // file I/O touches SIF
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return;       // no overrides yet -> auto-defaults stand
    }
    static char buf[64 * 1024];
    int total = 0, r = 0;
    while (total < (int)sizeof(buf) - 1 &&
           (r = (int)::read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
    }
    ::close(fd);
    buf[total] = '\0';

    // Lines "name=WxH": split on the LAST '=' (names may contain '='), resolve the name
    // to a game index and the size to a preset.
    int i = 0;
    while (i < total) {
        const int start = i;
        while (i < total && buf[i] != '\n' && buf[i] != '\r') ++i;
        const int len = i - start;
        while (i < total && (buf[i] == '\n' || buf[i] == '\r')) ++i;
        if (len <= 2) continue;

        char* line = &buf[start];
        const char saved = line[len];
        line[len] = '\0';
        char* eq = 0;
        for (char* c = line; *c != '\0'; ++c) {
            if (*c == '=') eq = c;
        }
        if (eq != 0) {
            *eq = '\0';
            int w = 0, h = 0;
            if (sscanf(eq + 1, "%dx%d", &w, &h) == 2) {
                const int pi = presetForSize(w, h);
                if (pi >= 0) {
                    for (int g = 0; g < gameCount_; ++g) {
                        const char* gn = hal::GameStorage::instance().nameAt(g);
                        if (gn != 0 && strcmp(gn, line) == 0) {
                            idx_[g] = (unsigned char)pi;
                            break;
                        }
                    }
                }
            }
        }
        line[len] = saved;
    }
}

int Resolutions::indexFor(int game) const {
    return (game >= 0 && game < gameCount_) ? idx_[game] : 0;
}

void Resolutions::get(int game, int* w, int* h) const {
    preset(indexFor(game), w, h);
}

void Resolutions::cycle(int game, int delta) {
    if (game < 0 || game >= gameCount_) {
        return;
    }
    int n = (int)idx_[game] + delta;
    while (n < 0) n += kPresetCount;
    n %= kPresetCount;
    idx_[game] = (unsigned char)n;
    save();
}

void Resolutions::save() const {
    char path[256];
    if (!Ps2Storage::instance().resolutionsPath(path, (int)sizeof(path))) {
        return;
    }
    SifGuard guard;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    // Persist only games that deviate from their filename auto-default.
    for (int g = 0; g < gameCount_; ++g) {
        if (idx_[g] == def_[g]) {
            continue;
        }
        const char* nm = hal::GameStorage::instance().nameAt(g);
        if (nm == 0 || nm[0] == '\0') {
            continue;
        }
        int w = 0, h = 0;
        preset(idx_[g], &w, &h);
        char line[320];
        const int n = snprintf(line, (int)sizeof(line), "%s=%dx%d\n", nm, w, h);
        if (n > 0) {
            ::write(fd, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
        }
    }
    ::close(fd);
}

} // namespace platform
} // namespace ps2
