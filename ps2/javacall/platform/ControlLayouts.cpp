// PS2 JavaCall port — platform layer. ControlLayouts implementation.
//
// Per-game controller mapping, stored by JAR name (one "name=Simple|Complete" line) in
// "controllayouts.txt", persisted on the memory card (falling back to the boot
// directory) via Ps2MemCard, resolved to game indices at load(). Games left on the
// Global default are not written. See ControlLayouts.hpp.
#include "ControlLayouts.hpp"

#include "Ps2MemCard.hpp"
#include "../hal/GameStorage.hpp"

#include <string.h>     // strcmp, memcpy
#include <stdio.h>      // snprintf

namespace ps2 {
namespace platform {

namespace {
const char* const kLabels[] = { "Global", "Simple", "Complete" };
const int kPresetCount = (int)(sizeof(kLabels) / sizeof(kLabels[0]));
} // namespace

ControlLayouts& ControlLayouts::instance() {
    static ControlLayouts inst;
    return inst;
}

ControlLayouts::ControlLayouts() : loaded_(false), gameCount_(0) {
    for (int i = 0; i < MAX_GAMES; ++i) {
        idx_[i] = GLOBAL;
    }
}

int ControlLayouts::presetCount() {
    return kPresetCount;
}

const char* ControlLayouts::preset(int i) {
    if (i < 0 || i >= kPresetCount) {
        i = 0;
    }
    return kLabels[i];
}

void ControlLayouts::load(int gameCount) {
    gameCount_ = gameCount < 0 ? 0 : (gameCount > MAX_GAMES ? MAX_GAMES : gameCount);
    loaded_ = true;

    for (int g = 0; g < gameCount_; ++g) {
        idx_[g] = GLOBAL;   // default: follow the global Settings value
    }

    static char buf[64 * 1024];
    int total = Ps2MemCard::instance().configRead("controllayouts.txt", buf, (int)sizeof(buf) - 1);
    if (total <= 0) {
        return;       // no overrides yet -> all Global
    }
    buf[total] = '\0';

    // Lines "name=Simple|Complete": split on the LAST '=' (names may contain '='),
    // resolve the name to a game index and the value to SIMPLE/COMPLETE.
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
            const char* val = eq + 1;
            int v = GLOBAL;
            if (strcmp(val, "Simple") == 0)        v = SIMPLE;
            else if (strcmp(val, "Complete") == 0) v = COMPLETE;
            if (v != GLOBAL) {
                for (int g = 0; g < gameCount_; ++g) {
                    const char* gn = hal::GameStorage::instance().nameAt(g);
                    if (gn != 0 && strcmp(gn, line) == 0) {
                        idx_[g] = (unsigned char)v;
                        break;
                    }
                }
            }
        }
        line[len] = saved;
    }
}

int ControlLayouts::indexFor(int game) const {
    return (game >= 0 && game < gameCount_) ? (int)idx_[game] : (int)GLOBAL;
}

void ControlLayouts::cycle(int game, int delta) {
    if (game < 0 || game >= gameCount_) {
        return;
    }
    int n = (int)idx_[game] + delta;
    while (n < 0) n += kPresetCount;
    n %= kPresetCount;
    idx_[game] = (unsigned char)n;
    save();
}

void ControlLayouts::save() const {
    // Build the whole file, then persist it in one write. Only games that deviate from
    // the Global default are stored.
    static char buf[64 * 1024];
    int len = 0;
    for (int g = 0; g < gameCount_; ++g) {
        if (idx_[g] == GLOBAL) {
            continue;
        }
        const char* nm = hal::GameStorage::instance().nameAt(g);
        if (nm == 0 || nm[0] == '\0') {
            continue;
        }
        char line[320];
        const int n = snprintf(line, (int)sizeof(line), "%s=%s\n", nm, kLabels[idx_[g]]);
        if (n <= 0 || len + n > (int)sizeof(buf)) {
            break;
        }
        memcpy(buf + len, line, n);
        len += n;
    }
    Ps2MemCard::instance().configWrite("controllayouts.txt", buf, len);
}

} // namespace platform
} // namespace ps2
